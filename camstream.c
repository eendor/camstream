/* camstream.c - laptop webcam + mic -> LAN stream for OBS (C, V4L2 + ALSA)
 * Build: gcc -O2 -Wall -pthread -o camstream camstream.c -lasound
 * Run:   camstream [/dev/video0] [port] [width] [height] [alsa-mic]
 * OBS:   Media Source, uncheck "Local File", Input: http://<ip>:8080/
 *        (video + mic audio, Matroska). http://<ip>:8080/mjpeg = video only.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <linux/videodev2.h>
#include <linux/sockios.h>
#include <alsa/asoundlib.h>

#define NBUF 2                 /* fewer camera buffers = less queued delay */
#define RING 128
#define RATE 48000
#define CHANS 2
#define AFRAMES 240            /* 5 ms of audio per packet */

enum { PKT_VIDEO = 1, PKT_AUDIO = 2 };
typedef struct { int type; int64_t ts; unsigned char *data; size_t len, cap; } Packet;

static struct { void *p; size_t len; } bufs[NBUF];
static Packet ring[RING];
static uint64_t ring_seq;               /* number of packets ever pushed */
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cnd = PTHREAD_COND_INITIALIZER;
static int vid_w, vid_h, have_audio;

static void die(const char *m) { perror(m); exit(1); }
static int xioctl(int fd, unsigned long req, void *arg) {
    int r; do r = ioctl(fd, req, arg); while (r < 0 && errno == EINTR); return r;
}
static int64_t now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void push_packet(int type, int64_t ts, const void *d, size_t n) {
    pthread_mutex_lock(&mtx);
    Packet *p = &ring[ring_seq % RING];
    if (n > p->cap) { p->cap = n * 2; p->data = realloc(p->data, p->cap); if (!p->data) die("realloc"); }
    memcpy(p->data, d, n); p->len = n; p->type = type; p->ts = ts;
    ring_seq++;
    pthread_cond_broadcast(&cnd);
    pthread_mutex_unlock(&mtx);
}

/* ---------- camera ---------- */
static int open_cam(const char *dev, int w, int h) {
    int fd = open(dev, O_RDWR);
    if (fd < 0) die("open camera");
    struct v4l2_format f = {0};
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    f.fmt.pix.width = w; f.fmt.pix.height = h;
    f.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    f.fmt.pix.field = V4L2_FIELD_ANY;
    if (xioctl(fd, VIDIOC_S_FMT, &f) < 0) die("VIDIOC_S_FMT");
    if (f.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG) { fprintf(stderr, "camera lacks MJPEG\n"); exit(1); }
    vid_w = f.fmt.pix.width; vid_h = f.fmt.pix.height;
    struct v4l2_streamparm sp = {0};
    sp.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    sp.parm.capture.timeperframe.numerator = 1;
    sp.parm.capture.timeperframe.denominator = 30;
    xioctl(fd, VIDIOC_S_PARM, &sp);
    /* keep a steady 30 fps: stop auto-exposure from slowing frames in dim light */
    struct v4l2_control ctl = { V4L2_CID_EXPOSURE_AUTO_PRIORITY, 0 };
    xioctl(fd, VIDIOC_S_CTRL, &ctl);
    printf("Camera: %dx%d MJPEG\n", vid_w, vid_h);

    struct v4l2_requestbuffers rb = {0};
    rb.count = NBUF; rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; rb.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &rb) < 0) die("VIDIOC_REQBUFS");
    for (unsigned i = 0; i < rb.count && i < NBUF; i++) {
        struct v4l2_buffer b = {0};
        b.type = rb.type; b.memory = V4L2_MEMORY_MMAP; b.index = i;
        if (xioctl(fd, VIDIOC_QUERYBUF, &b) < 0) die("VIDIOC_QUERYBUF");
        bufs[i].len = b.length;
        bufs[i].p = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset);
        if (bufs[i].p == MAP_FAILED) die("mmap");
        if (xioctl(fd, VIDIOC_QBUF, &b) < 0) die("VIDIOC_QBUF");
    }
    enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd, VIDIOC_STREAMON, &t) < 0) die("VIDIOC_STREAMON");
    return fd;
}

static void *video_loop(void *arg) {
    int fd = *(int *)arg;
    for (;;) {
        struct v4l2_buffer b = {0};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; b.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd, VIDIOC_DQBUF, &b) < 0) die("VIDIOC_DQBUF");
        push_packet(PKT_VIDEO, now_ms(), bufs[b.index].p, b.bytesused);
        if (xioctl(fd, VIDIOC_QBUF, &b) < 0) die("VIDIOC_QBUF");
    }
    return NULL;
}

/* ---------- microphone ---------- */
static snd_pcm_t *open_mic(const char *dev) {
    snd_pcm_t *pcm; int e;
    if ((e = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_CAPTURE, 0)) < 0) {
        fprintf(stderr, "Mic '%s': %s (continuing without audio)\n", dev, snd_strerror(e));
        return NULL;
    }
    if ((e = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                                CHANS, RATE, 1, 20000)) < 0) {
        fprintf(stderr, "Mic setup: %s (continuing without audio)\n", snd_strerror(e));
        snd_pcm_close(pcm); return NULL;
    }
    printf("Mic: %s, %d Hz %s\n", dev, RATE, CHANS == 2 ? "stereo" : "mono");
    return pcm;
}

static void *audio_loop(void *arg) {
    snd_pcm_t *pcm = arg;
    static int16_t buf[AFRAMES * CHANS];
    int64_t t0 = -1; uint64_t total = 0;
    for (;;) {
        snd_pcm_sframes_t n = snd_pcm_readi(pcm, buf, AFRAMES);
        if (n < 0) { snd_pcm_recover(pcm, (int)n, 1); t0 = -1; continue; }
        /* timestamp from sample count keeps audio smooth; resync after overruns */
        if (t0 < 0) { t0 = now_ms() - n * 1000 / RATE; total = 0; }
        push_packet(PKT_AUDIO, t0 + (int64_t)(total * 1000 / RATE), buf, n * CHANS * 2);
        total += n;
    }
    return NULL;
}

/* ---------- tiny Matroska (MKV) writer ---------- */
typedef struct { unsigned char *p; size_t len, cap; } Buf;
static void b_put(Buf *b, const void *d, size_t n) {
    if (b->len + n > b->cap) { b->cap = (b->len + n) * 2; b->p = realloc(b->p, b->cap); if (!b->p) die("realloc"); }
    memcpy(b->p + b->len, d, n); b->len += n;
}
static void b_id(Buf *b, uint32_t id) {
    unsigned char t[4]; int n = id > 0xFFFFFF ? 4 : id > 0xFFFF ? 3 : id > 0xFF ? 2 : 1;
    for (int i = 0; i < n; i++) t[i] = id >> (8 * (n - 1 - i));
    b_put(b, t, n);
}
static void b_size(Buf *b, uint64_t s) {      /* always 8-byte EBML size */
    unsigned char t[8] = { 0x01 };
    for (int i = 1; i < 8; i++) t[i] = s >> (8 * (7 - i));
    b_put(b, t, 8);
}
static void e_uint(Buf *b, uint32_t id, uint64_t v) {
    unsigned char t[8]; int n = 8;
    while (n > 1 && !(v >> (8 * (n - 1)))) n--;
    for (int i = 0; i < n; i++) t[i] = v >> (8 * (n - 1 - i));
    b_id(b, id); b_size(b, n); b_put(b, t, n);
}
static void e_str(Buf *b, uint32_t id, const char *s) { b_id(b, id); b_size(b, strlen(s)); b_put(b, s, strlen(s)); }
static void e_float(Buf *b, uint32_t id, double d) {
    uint64_t v; memcpy(&v, &d, 8); unsigned char t[8];
    for (int i = 0; i < 8; i++) t[i] = v >> (8 * (7 - i));
    b_id(b, id); b_size(b, 8); b_put(b, t, 8);
}
static void e_master(Buf *b, uint32_t id, Buf *child) { b_id(b, id); b_size(b, child->len); b_put(b, child->p, child->len); child->len = 0; }

static void mkv_header(Buf *out) {
    Buf c = {0}, t = {0}, s = {0}, tr = {0};
    e_uint(&c, 0x4286, 1); e_uint(&c, 0x42F7, 1); e_uint(&c, 0x42F2, 4); e_uint(&c, 0x42F3, 8);
    e_str(&c, 0x4282, "matroska"); e_uint(&c, 0x4287, 4); e_uint(&c, 0x4285, 2);
    e_master(out, 0x1A45DFA3, &c);
    b_id(out, 0x18538067);                                   /* Segment, unknown size (live) */
    { unsigned char u[8] = {0x01,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF}; b_put(out, u, 8); }
    e_uint(&c, 0x2AD7B1, 1000000); e_str(&c, 0x4D80, "camstream"); e_str(&c, 0x5741, "camstream");
    e_master(out, 0x1549A966, &c);                           /* Info: 1 ms timecodes */
    /* video track */
    e_uint(&t, 0xD7, 1); e_uint(&t, 0x73C5, 1); e_uint(&t, 0x83, 1); e_uint(&t, 0x9C, 0);
    e_str(&t, 0x86, "V_MJPEG");
    e_uint(&t, 0x23E383, 33333333);                          /* DefaultDuration: 30 fps */
    e_uint(&s, 0xB0, vid_w); e_uint(&s, 0xBA, vid_h); e_master(&t, 0xE0, &s);
    e_master(&tr, 0xAE, &t);
    if (have_audio) {
        e_uint(&t, 0xD7, 2); e_uint(&t, 0x73C5, 2); e_uint(&t, 0x83, 2); e_uint(&t, 0x9C, 0);
        e_str(&t, 0x86, "A_PCM/INT/LIT");
        e_float(&s, 0xB5, RATE); e_uint(&s, 0x9F, CHANS); e_uint(&s, 0x6264, 16); e_master(&t, 0xE1, &s);
        e_master(&tr, 0xAE, &t);
    }
    e_master(out, 0x1654AE6B, &tr);
    free(c.p); free(t.p); free(s.p); free(tr.p);
}

/* one Cluster per packet. Cluster times must never go backwards, so a packet that is
 * slightly older than the last cluster gets a small negative offset inside its block. */
static void mkv_packet(Buf *out, const Packet *p, int64_t ts, int64_t *last) {
    Buf c = {0};
    int64_t ct = ts > *last ? ts : *last;
    int16_t rel = (int16_t)(ts - ct);
    *last = ct;
    unsigned char hdr[4] = { (unsigned char)(0x80 | p->type), (unsigned char)((uint16_t)rel >> 8),
                             (unsigned char)(rel & 0xFF), 0x80 };   /* track, rel ts, keyframe */
    e_uint(&c, 0xE7, (uint64_t)ct);
    b_id(&c, 0xA3); b_size(&c, 4 + p->len); b_put(&c, hdr, 4); b_put(&c, p->data, p->len);
    e_master(out, 0x1F43B675, &c);
    free(c.p);
}

/* ---------- HTTP ---------- */
static int send_all(int s, const void *d, size_t n) {
    const char *p = d;
    while (n) {
        ssize_t r = send(s, p, n, MSG_NOSIGNAL);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; return -1; }
        p += r; n -= r;
    }
    return 0;
}

/* wait for the next packet after *seq; copies it into *dst. skips ahead if too slow */
static void next_packet(uint64_t *seq, Packet *dst) {
    pthread_mutex_lock(&mtx);
    while (*seq >= ring_seq) pthread_cond_wait(&cnd, &mtx);
    if (ring_seq - *seq > 12) *seq = ring_seq - 1;          /* behind? drop to newest (low latency) */
    Packet *p = &ring[*seq % RING];
    if (p->len > dst->cap) { dst->cap = p->cap; dst->data = realloc(dst->data, dst->cap); }
    memcpy(dst->data, p->data, p->len); dst->len = p->len; dst->type = p->type; dst->ts = p->ts;
    (*seq)++;
    pthread_mutex_unlock(&mtx);
}

/* true if the viewer still hasn't received most of the previous frame:
 * then we skip this frame instead of queueing it (keeps latency low) */
static int viewer_behind(int s, size_t frame_len) {
    int queued = 0;
    return ioctl(s, SIOCOUTQ, &queued) == 0 && (size_t)queued > frame_len / 2;
}

static void *client_loop(void *arg) {
    int s = (int)(long)arg;
    char req[1024] = {0}; recv(s, req, sizeof req - 1, 0);
    int mjpeg = strncmp(req, "GET /mjpeg", 10) == 0;
    int audio = strncmp(req, "GET /audio", 10) == 0;
    Packet pk = {0}; Buf out = {0};
    pthread_mutex_lock(&mtx); uint64_t seq = ring_seq; pthread_mutex_unlock(&mtx);

    if (audio) {
        if (!have_audio) { send_all(s, "HTTP/1.0 503 No mic\r\n\r\n", 23); goto out; }
        const char *h = "HTTP/1.0 200 OK\r\nCache-Control: no-cache\r\nConnection: close\r\n"
                        "Content-Type: audio/wav\r\n\r\n";
        if (send_all(s, h, strlen(h)) < 0) goto out;
        /* WAV header with "endless" sizes for a live stream */
        unsigned char w[44] = "RIFF\xff\xff\xff\xffWAVEfmt \x10\0\0\0\x01\0";
        uint32_t rate = RATE, brate = RATE * CHANS * 2;
        w[22] = CHANS; w[23] = 0;
        memcpy(w + 24, &rate, 4); memcpy(w + 28, &brate, 4);
        w[32] = CHANS * 2; w[33] = 0; w[34] = 16; w[35] = 0;
        memcpy(w + 36, "data\xff\xff\xff\xff", 8);
        if (send_all(s, w, 44) < 0) goto out;
        for (;;) {
            next_packet(&seq, &pk);
            if (pk.type != PKT_AUDIO) continue;
            int queued = 0;   /* more than ~60 ms waiting? drop to keep latency low */
            if (ioctl(s, SIOCOUTQ, &queued) == 0 && queued > RATE * CHANS * 2 * 60 / 1000) continue;
            if (send_all(s, pk.data, pk.len) < 0) break;
        }
    } else if (mjpeg) {
        const char *h = "HTTP/1.0 200 OK\r\nCache-Control: no-cache\r\nConnection: close\r\n"
                        "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n";
        if (send_all(s, h, strlen(h)) < 0) goto out;
        for (;;) {
            next_packet(&seq, &pk);
            if (pk.type != PKT_VIDEO || viewer_behind(s, pk.len)) continue;
            char ph[128];
            int n = snprintf(ph, sizeof ph, "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n", pk.len);
            if (send_all(s, ph, n) < 0 || send_all(s, pk.data, pk.len) < 0 || send_all(s, "\r\n", 2) < 0) break;
        }
    } else {
        const char *h = "HTTP/1.0 200 OK\r\nCache-Control: no-cache\r\nConnection: close\r\n"
                        "Content-Type: video/x-matroska\r\n\r\n";
        if (send_all(s, h, strlen(h)) < 0) goto out;
        mkv_header(&out);
        if (send_all(s, out.p, out.len) < 0) goto out;
        int64_t base = -1, last = 0;
        for (;;) {
            next_packet(&seq, &pk);
            if (base < 0) { if (pk.type != PKT_VIDEO) continue; base = pk.ts; } /* start on a frame */
            if (pk.ts < base) continue;
            if (pk.type == PKT_VIDEO && viewer_behind(s, pk.len)) continue;
            out.len = 0;
            if (pk.ts - base < last - 1000) continue;    /* drop anything far too old */
            mkv_packet(&out, &pk, pk.ts - base, &last);
            if (send_all(s, out.p, out.len) < 0) break;
        }
    }
out:
    free(pk.data); free(out.p); close(s);
    puts("Client disconnected");
    return NULL;
}

int main(int argc, char **argv) {
    const char *dev = argc > 1 ? argv[1] : "/dev/video0";
    int port = argc > 2 ? atoi(argv[2]) : 8080;
    int w = argc > 3 ? atoi(argv[3]) : 1280;
    int h = argc > 4 ? atoi(argv[4]) : 720;
    const char *mic = argc > 5 ? argv[5] : "default";
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IOLBF, 0);

    static int camfd; camfd = open_cam(dev, w, h);
    snd_pcm_t *pcm = open_mic(mic);
    have_audio = pcm != NULL;
    pthread_t t;
    pthread_create(&t, NULL, video_loop, &camfd);
    if (pcm) pthread_create(&t, NULL, audio_loop, pcm);

    int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = INADDR_ANY;
    if (bind(ls, (struct sockaddr *)&a, sizeof a) < 0) die("bind");
    if (listen(ls, 8) < 0) die("listen");

    puts("Streaming! Use one of these URLs in OBS:");
    struct ifaddrs *ifs, *i;
    if (getifaddrs(&ifs) == 0) {
        for (i = ifs; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
            char ipb[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, ipb, sizeof ipb);
            if (strcmp(ipb, "127.0.0.1") == 0) continue;
            printf("  http://%s:%d/        video%s (Media Source)\n", ipb, port, have_audio ? " + mic" : "");
            printf("  http://%s:%d/mjpeg   video only, lowest delay (Browser Source)\n", ipb, port);
            if (have_audio) printf("  http://%s:%d/audio   mic only (Media Source)\n", ipb, port);
        }
        freeifaddrs(ifs);
    }
    puts("Ctrl+C to stop");

    for (;;) {
        struct sockaddr_in ca; socklen_t cl = sizeof ca;
        int s = accept(ls, (struct sockaddr *)&ca, &cl);
        if (s < 0) continue;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        int sndbuf = 256 * 1024;   /* small send buffer so stale frames don't pile up */
        setsockopt(s, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof sndbuf);
        char ipb[INET_ADDRSTRLEN]; inet_ntop(AF_INET, &ca.sin_addr, ipb, sizeof ipb);
        printf("Client %s connected\n", ipb);
        pthread_create(&t, NULL, client_loop, (void *)(long)s);
        pthread_detach(t);
    }
}
