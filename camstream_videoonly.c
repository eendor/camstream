/* camstream.c - V4L2 MJPEG webcam -> HTTP MJPEG stream (pure C, no deps)
 * Build: gcc -O2 -Wall -pthread -o camstream camstream.c
 * Run:   ./camstream [/dev/video0] [port] [width] [height]
 *        On WiFi, lower the resolution (e.g. 640x480) to cut bitrate and latency.
 * OBS:   Media Source, uncheck "Local File", Input: http://<ip>:8080/
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <linux/videodev2.h>
#include <ifaddrs.h>
#include <arpa/inet.h>

#define NBUF 4

static struct { void *p; size_t len; } bufs[NBUF];
static unsigned char *frame; static size_t frame_len, frame_cap;
static unsigned long frame_seq;
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cnd = PTHREAD_COND_INITIALIZER;

static int xioctl(int fd, unsigned long req, void *arg) {
    int r; do r = ioctl(fd, req, arg); while (r < 0 && errno == EINTR); return r;
}
static void die(const char *m) { perror(m); exit(1); }

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
    struct v4l2_streamparm sp = {0};
    sp.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    sp.parm.capture.timeperframe.numerator = 1;
    sp.parm.capture.timeperframe.denominator = 30;
    xioctl(fd, VIDIOC_S_PARM, &sp);
    printf("Camera: %ux%u MJPEG\n", f.fmt.pix.width, f.fmt.pix.height);

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

static void *capture_loop(void *arg) {
    int fd = *(int *)arg;
    for (;;) {
        struct v4l2_buffer b = {0};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; b.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd, VIDIOC_DQBUF, &b) < 0) die("VIDIOC_DQBUF");
        pthread_mutex_lock(&mtx);
        if (b.bytesused > frame_cap) {
            frame_cap = b.bytesused * 2;
            frame = realloc(frame, frame_cap);
            if (!frame) die("realloc");
        }
        memcpy(frame, bufs[b.index].p, b.bytesused);
        frame_len = b.bytesused; frame_seq++;
        pthread_cond_broadcast(&cnd);
        pthread_mutex_unlock(&mtx);
        if (xioctl(fd, VIDIOC_QBUF, &b) < 0) die("VIDIOC_QBUF");
    }
    return NULL;
}

static int send_all(int s, const void *d, size_t n) {
    const char *p = d;
    while (n) {
        ssize_t r = send(s, p, n, MSG_NOSIGNAL);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; return -1; }
        p += r; n -= r;
    }
    return 0;
}

static void *client_loop(void *arg) {
    int s = (int)(long)arg;
    char req[1024]; recv(s, req, sizeof req, 0); /* ignore request contents */
    const char *hdr =
        "HTTP/1.0 200 OK\r\n"
        "Cache-Control: no-cache\r\nConnection: close\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n";
    unsigned char *local = NULL; size_t lcap = 0, llen; unsigned long seen = 0;
    if (send_all(s, hdr, strlen(hdr)) < 0) goto out;
    for (;;) {
        pthread_mutex_lock(&mtx);
        while (frame_seq == seen) pthread_cond_wait(&cnd, &mtx);
        seen = frame_seq;
        if (frame_len > lcap) { lcap = frame_cap; local = realloc(local, lcap); }
        memcpy(local, frame, frame_len); llen = frame_len;
        pthread_mutex_unlock(&mtx);
        char ph[128];
        int n = snprintf(ph, sizeof ph,
            "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n", llen);
        if (send_all(s, ph, n) < 0 || send_all(s, local, llen) < 0 ||
            send_all(s, "\r\n", 2) < 0) break;
    }
out:
    free(local); close(s);
    puts("Client disconnected");
    return NULL;
}

int main(int argc, char **argv) {
    const char *dev = argc > 1 ? argv[1] : "/dev/video0";
    int port = argc > 2 ? atoi(argv[2]) : 8080;
    int w = argc > 3 ? atoi(argv[3]) : 640;
    int h = argc > 4 ? atoi(argv[4]) : 480;
    signal(SIGPIPE, SIG_IGN);

    static int camfd; camfd = open_cam(dev, w, h);
    pthread_t ct; pthread_create(&ct, NULL, capture_loop, &camfd);

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
            printf("  http://%s:%d/   (%s)\n", ipb, port, i->ifa_name);
        }
        freeifaddrs(ifs);
    }
    puts("Ctrl+C to stop");
    for (;;) {
        struct sockaddr_in ca; socklen_t cl = sizeof ca;
        int s = accept(ls, (struct sockaddr *)&ca, &cl);
        if (s < 0) continue;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        int sndbuf = 64 * 1024;   /* small send buffer so stale frames don't pile up on WiFi */
        setsockopt(s, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof sndbuf);
        unsigned char *ip = (unsigned char *)&ca.sin_addr;
        printf("Client %u.%u.%u.%u connected\n", ip[0], ip[1], ip[2], ip[3]);
        pthread_t t; pthread_create(&t, NULL, client_loop, (void *)(long)s);
        pthread_detach(t);
    }
}
