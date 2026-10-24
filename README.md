# camstream 📹

Ultra-low-latency Linux webcam + microphone streamer over LAN designed specifically for OBS Studio, written in pure C using Video4Linux2 (V4L2) and ALSA.

Packages live MJPEG video and ALSA audio directly into a lightweight Matroska (MKV) container or pure multipart MJPEG stream over HTTP without heavy external multimedia frameworks (like GStreamer or FFmpeg).

---

## Features

- **Near-Zero Latency**: Direct memory-mapped V4L2 buffers (`NBUF 2`) with ring-buffered ALSA audio packets to avoid queue lag.
- **Audio + Video in MKV**: Streams Matroska container with MJPEG video + PCM audio (48kHz stereo, S16LE) over HTTP.
- **Standalone Video Endpoint**: Supports pure multipart MJPEG stream at `/mjpeg` viewable in web browsers and VLC.
- **Zero Heavy Dependencies**: Written in standard C with only `libasound` (ALSA) required.
- **Multiple Video Sources**: Configurable device path, resolution, and audio device.

---

## Prerequisites

- Linux with V4L2 camera support
- GCC / Clang and `make`
- ALSA development library (`libasound2-dev` on Debian/Ubuntu, `alsa-lib` on Arch Linux)

On Arch Linux:
```bash
sudo pacman -S base-devel alsa-lib
```

On Ubuntu / Debian:
```bash
sudo apt update
sudo apt install build-essential libasound2-dev
```

---

## Building

```bash
make
```

To build only the audio-less video streamer:
```bash
make camstream_videoonly
```

To install to `~/.local/bin`:
```bash
make install
```

---

## Usage

```bash
./camstream [device] [port] [width] [height] [alsa-mic]
```

### Defaults:
- `device`: `/dev/video0`
- `port`: `8080`
- `width`: `1280`
- `height`: `720`
- `alsa-mic`: `default`

### Examples:
```bash
# Default settings (720p @ /dev/video0 on port 8080)
./camstream

# Custom resolution and port
./camstream /dev/video0 8080 1920 1080 default
```

---

## Adding to OBS Studio

1. In OBS, add a new **Media Source** to your scene.
2. Uncheck **Local File**.
3. In **Input**, enter:
   ```
   http://<YOUR_LINUX_IP>:8080/
   ```
4. (Optional) Set **Network Buffering** to minimal for lowest latency.
5. For video-only, use:
   ```
   http://<YOUR_LINUX_IP>:8080/mjpeg
   ```

---

## License

MIT License. See [LICENSE](LICENSE) for details.
