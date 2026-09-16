/* SPDX-License-Identifier: MIT */

#include <getopt.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tpw/tpw_stream.h"

static volatile sig_atomic_t g_running = 1;

static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

/* Single-producer/single-consumer byte ring: the audio capture stream (one
 * real-time thread) writes, the playback stream (a different real-time
 * thread) reads. write_pos/read_pos only ever grow, wrapping into `data`
 * via modulo, so neither side needs to touch the other's index. */
typedef struct {
    uint8_t* data;
    size_t capacity;
    atomic_size_t write_pos;
    atomic_size_t read_pos;
    atomic_size_t dropped;
} loopback_ring;

static bool ring_init(loopback_ring* ring, size_t capacity)
{
    ring->data = calloc(1, capacity);
    if (!ring->data)
        return false;
    ring->capacity = capacity;
    atomic_init(&ring->write_pos, (size_t)0);
    atomic_init(&ring->read_pos, (size_t)0);
    atomic_init(&ring->dropped, (size_t)0);
    return true;
}

static void ring_destroy(loopback_ring* ring)
{
    free(ring->data);
}

/* Capture thread only. Drops the newest bytes on overrun rather than
 * advancing read_pos itself, which only the playback thread may do. */
static void ring_write(loopback_ring* ring, const uint8_t* src, size_t len)
{
    size_t read_pos = atomic_load_explicit(&ring->read_pos, memory_order_acquire);
    size_t write_pos = atomic_load_explicit(&ring->write_pos, memory_order_relaxed);
    size_t free_space = ring->capacity - (write_pos - read_pos);
    if (len > free_space) {
        atomic_fetch_add_explicit(&ring->dropped, len - free_space, memory_order_relaxed);
        len = free_space;
    }
    for (size_t i = 0; i < len; i++)
        ring->data[(write_pos + i) % ring->capacity] = src[i];
    atomic_store_explicit(&ring->write_pos, write_pos + len, memory_order_release);
}

/* Playback thread only. Returns fewer bytes than `len` when the ring is
 * running low; the caller leaves the remainder as silence. */
static size_t ring_read(loopback_ring* ring, uint8_t* dst, size_t len)
{
    size_t write_pos = atomic_load_explicit(&ring->write_pos, memory_order_acquire);
    size_t read_pos = atomic_load_explicit(&ring->read_pos, memory_order_relaxed);
    size_t available = write_pos - read_pos;
    if (len > available)
        len = available;
    for (size_t i = 0; i < len; i++)
        dst[i] = ring->data[(read_pos + i) % ring->capacity];
    atomic_store_explicit(&ring->read_pos, read_pos + len, memory_order_release);
    return len;
}

static void on_audio_capture(tpw_stream_h stream, const tpw_stream_buffer* buf, void* user_data)
{
    (void)stream;
    if (buf->data && buf->size > 0)
        ring_write(user_data, buf->data, buf->size);
}

/* Real-time thread: must not block, allocate, or perform I/O, so this is
 * the ring read and nothing else. */
static void on_audio_playback(tpw_stream_h stream, tpw_stream_playback_buffer* buf, void* user_data)
{
    (void)stream;
    buf->size = ring_read(user_data, buf->data, buf->available);
}

static void on_audio_capture_error(tpw_stream_h stream, int error_code, void* user_data)
{
    (void)stream;
    (void)user_data;
    fprintf(stderr, "tpw_stream_loopback: audio capture source lost (error %d)\n", error_code);
    g_running = 0;
}

static void on_audio_playback_error(tpw_stream_h stream, int error_code, void* user_data)
{
    (void)stream;
    (void)user_data;
    fprintf(stderr, "tpw_stream_loopback: audio output device lost (error %d)\n", error_code);
    g_running = 0;
}

typedef struct {
    int index;
    bool dmabuf;
    uint64_t frames;
} video_ctx;

static void on_video_data(tpw_stream_h stream, const tpw_stream_buffer* buf, void* user_data)
{
    video_ctx* ctx = user_data;
    ctx->frames++;

    if (!ctx->dmabuf) {
        fprintf(stderr, "video[%d]: frame %llu size=%zu (pts=%lld ns)\n", ctx->index,
                (unsigned long long)ctx->frames, buf->size, (long long)buf->pts);
        return;
    }

    tpw_dmabuf_plane planes[4];
    size_t n_planes = tpw_stream_get_dmabuf_planes(stream, planes, 4);
    if (n_planes == 0) {
        fprintf(stderr, "video[%d]: frame %llu, no DMABUF plane yet (pts=%lld ns)\n", ctx->index,
                (unsigned long long)ctx->frames, (long long)buf->pts);
        return;
    }
    fprintf(stderr, "video[%d]: frame %llu, %zu plane(s) fd=%d stride=%u size=%u (pts=%lld ns)\n", ctx->index,
            (unsigned long long)ctx->frames, n_planes, planes[0].fd, planes[0].stride, planes[0].size,
            (long long)buf->pts);
}

static void on_video_error(tpw_stream_h stream, int error_code, void* user_data)
{
    (void)stream;
    video_ctx* ctx = user_data;
    fprintf(stderr, "video[%d]: source lost (error %d)\n", ctx->index, error_code);
    g_running = 0;
}

/* Maps --bits's value to tpw_audio_config.format and its on-disk sample
 * width, mirroring tpw_record's table. */
static const struct {
    const char* arg;
    const char* tpw_format;
    int bytes_per_sample;
} audio_bit_depths[] = {
    { "16", "S16", 2 },
    { "24", "S24", 3 },
    { "32", "S32", 4 },
    { "f32", "F32", 4 },
};

static void print_usage(const char* prog)
{
    fprintf(stderr,
        "usage: %s [options]\n"
        "\n"
        "  Loops captured audio straight to the default output device, and/or\n"
        "  logs data callbacks from one or more video capture streams. Uses only\n"
        "  the tpw_stream API (no filter graph).\n"
        "\n"
        "  -d, --duration <seconds>  stop after N seconds (default: run until Ctrl+C)\n"
        "  -h, --help                show this help\n"
        "\n"
        "  audio (on by default):\n"
        "      --no-audio           disable the audio loopback\n"
        "      --device <name>      capture node name or serial (see `wpctl status`);\n"
        "                           a name no node matches falls back to the default\n"
        "      --sample-rate <hz>   capture/playback sample rate (default: 48000)\n"
        "      --channels <n>       channel count (default: 2)\n"
        "      --bits 16|24|32|f32  sample format: signed int bit depth, or f32 for\n"
        "                           32-bit float (default: 16)\n"
        "\n"
        "  video (off by default):\n"
        "      --video-streams <n>  number of video capture streams to open (default: 0)\n"
        "      --dmabuf             negotiate DMABUF frames instead of CPU-mapped\n"
        "      --pixel-format <fmt> RGB, YUYV, NV12, NV21, I420, or MJPG (default: YUYV;\n"
        "                           the source must support it, no conversion)\n"
        "      --fps <n>            frame rate; 0 lets the source pick it (default: 0)\n"
        "      --width <px>         frame width (default: 640)\n"
        "      --height <px>        frame height (default: 480)\n"
        "\n"
        "  Video streams only log their data callback for now; rendering lands\n"
        "  once a UI backend option is added.\n",
        prog);
}

int main(int argc, char** argv)
{
    bool audio_enabled = true;
    const char* device = NULL;
    int duration = 0;
    int sample_rate = 48000;
    int channels = 2;
    const char* audio_format = NULL; /* tpw_audio_config.format; NULL = library default "S16" */
    int bytes_per_sample = 2;

    int video_streams_n = 0;
    bool use_dmabuf = false;
    const char* pixel_format = "YUYV";
    int fps = 0;
    int width = 640;
    int height = 480;

    static const struct option long_options[] = {
        { "duration", required_argument, NULL, 'd' },
        { "help", no_argument, NULL, 'h' },
        { "no-audio", no_argument, NULL, 1 },
        { "device", required_argument, NULL, 2 },
        { "sample-rate", required_argument, NULL, 3 },
        { "channels", required_argument, NULL, 4 },
        { "bits", required_argument, NULL, 5 },
        { "video-streams", required_argument, NULL, 6 },
        { "dmabuf", no_argument, NULL, 7 },
        { "pixel-format", required_argument, NULL, 8 },
        { "fps", required_argument, NULL, 9 },
        { "width", required_argument, NULL, 10 },
        { "height", required_argument, NULL, 11 },
        { NULL, 0, NULL, 0 }
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "d:h", long_options, NULL)) != -1) {
        switch (opt) {
        case 'd':
            duration = atoi(optarg);
            if (duration <= 0) {
                fprintf(stderr, "tpw_stream_loopback: --duration must be a positive integer\n");
                return 1;
            }
            break;
        case 1:
            audio_enabled = false;
            break;
        case 2:
            device = optarg;
            break;
        case 3:
            sample_rate = atoi(optarg);
            break;
        case 4:
            channels = atoi(optarg);
            break;
        case 5: {
            const size_t n = sizeof(audio_bit_depths) / sizeof(audio_bit_depths[0]);
            size_t i;
            for (i = 0; i < n; i++) {
                if (strcmp(optarg, audio_bit_depths[i].arg) == 0)
                    break;
            }
            if (i == n) {
                fprintf(stderr, "tpw_stream_loopback: --bits must be 16, 24, 32, or f32\n");
                return 1;
            }
            audio_format = audio_bit_depths[i].tpw_format;
            bytes_per_sample = audio_bit_depths[i].bytes_per_sample;
            break;
        }
        case 6:
            video_streams_n = atoi(optarg);
            if (video_streams_n < 0) {
                fprintf(stderr, "tpw_stream_loopback: --video-streams must not be negative\n");
                return 1;
            }
            break;
        case 7:
            use_dmabuf = true;
            break;
        case 8:
            pixel_format = optarg;
            break;
        case 9:
            fps = atoi(optarg);
            break;
        case 10:
            width = atoi(optarg);
            break;
        case 11:
            height = atoi(optarg);
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    if (!audio_enabled && video_streams_n == 0) {
        fprintf(stderr, "tpw_stream_loopback: nothing to do (--no-audio and no --video-streams)\n\n");
        print_usage(argv[0]);
        return 1;
    }

    signal(SIGINT, on_signal);
    if (duration > 0) {
        signal(SIGALRM, on_signal);
        alarm((unsigned)duration);
    }

    int status = 0;
    loopback_ring ring = { 0 };
    tpw_stream_h audio_capture = NULL;
    tpw_stream_h audio_playback = NULL;
    tpw_stream_h* video_streams = NULL;
    video_ctx* video_ctxs = NULL;
    int video_started = 0;

    if (audio_enabled) {
        size_t ring_capacity = (size_t)sample_rate * (size_t)channels * (size_t)bytes_per_sample;
        if (!ring_init(&ring, ring_capacity)) {
            fprintf(stderr, "tpw_stream_loopback: failed to allocate the audio ring buffer\n");
            status = 1;
            goto cleanup;
        }

        audio_capture = tpw_stream_create(TPW_DATA_AUDIO, on_audio_capture, &ring);
        if (!audio_capture) {
            fprintf(stderr, "tpw_stream_loopback: failed to create the audio capture stream (is PipeWire running?)\n");
            status = 1;
            goto cleanup;
        }
        tpw_stream_set_error_cb(audio_capture, on_audio_capture_error);
        if (device && tpw_stream_set_target(audio_capture, device) != TPW_OK) {
            fprintf(stderr, "tpw_stream_loopback: failed to select capture device '%s'\n", device);
            status = 1;
            goto cleanup;
        }

        tpw_audio_config audio_cfg = { .sample_rate = sample_rate, .channels = channels, .format = audio_format };
        if (tpw_stream_set_audio_config(audio_capture, &audio_cfg) != TPW_OK) {
            fprintf(stderr, "tpw_stream_loopback: failed to set the audio capture format\n");
            status = 1;
            goto cleanup;
        }

        audio_playback = tpw_stream_create_playback(on_audio_playback, &ring);
        if (!audio_playback) {
            fprintf(stderr, "tpw_stream_loopback: failed to create the audio playback stream\n");
            status = 1;
            goto cleanup;
        }
        tpw_stream_set_error_cb(audio_playback, on_audio_playback_error);
        if (tpw_stream_set_audio_config(audio_playback, &audio_cfg) != TPW_OK) {
            fprintf(stderr, "tpw_stream_loopback: failed to set the audio playback format\n");
            status = 1;
            goto cleanup;
        }

        if (tpw_stream_start(audio_capture) != TPW_OK || tpw_stream_start(audio_playback) != TPW_OK) {
            fprintf(stderr, "tpw_stream_loopback: failed to start the audio loopback\n");
            status = 1;
            goto cleanup;
        }
    }

    if (video_streams_n > 0) {
        video_streams = calloc((size_t)video_streams_n, sizeof(*video_streams));
        video_ctxs = calloc((size_t)video_streams_n, sizeof(*video_ctxs));
        if (!video_streams || !video_ctxs) {
            fprintf(stderr, "tpw_stream_loopback: failed to allocate video stream state\n");
            status = 1;
            goto cleanup;
        }

        tpw_video_config video_cfg = { .width = width, .height = height, .pixel_format = pixel_format, .fps = fps };
        tpw_stream_dmabuf_opts dmabuf_opts = { .memory = TPW_PORT_MEMORY_DMABUF };

        for (int i = 0; i < video_streams_n; i++) {
            video_ctxs[i].index = i;
            video_ctxs[i].dmabuf = use_dmabuf;

            video_streams[i] = tpw_stream_create(TPW_DATA_VIDEO, on_video_data, &video_ctxs[i]);
            if (!video_streams[i]) {
                fprintf(stderr, "tpw_stream_loopback: failed to create video stream %d\n", i);
                video_started = i;
                status = 1;
                goto cleanup;
            }
            tpw_stream_set_error_cb(video_streams[i], on_video_error);

            int cfg_res = use_dmabuf ? tpw_stream_set_video_config_ex(video_streams[i], &video_cfg, &dmabuf_opts)
                                      : tpw_stream_set_video_config(video_streams[i], &video_cfg);
            if (cfg_res != TPW_OK) {
                fprintf(stderr, "tpw_stream_loopback: failed to set video format for stream %d\n", i);
                video_started = i + 1;
                status = 1;
                goto cleanup;
            }

            if (tpw_stream_start(video_streams[i]) != TPW_OK) {
                fprintf(stderr, "tpw_stream_loopback: failed to start video stream %d\n", i);
                video_started = i + 1;
                status = 1;
                goto cleanup;
            }
            video_started = i + 1;
        }
    }

    if (audio_enabled) {
        fprintf(stderr, "tpw_stream_loopback: audio loopback active (%dHz, %dch, %s)\n", sample_rate, channels,
                audio_format ? audio_format : "S16");
        /* The target is only a hint: an unknown name leaves the session
         * manager free to pick the default source instead. */
        if (device)
            fprintf(stderr,
                    "tpw_stream_loopback: capture device requested: '%s' (a name no node matches\n"
                    "                     falls back to the default source; verify with `pw-link -l`)\n",
                    device);
    }
    if (video_streams_n > 0)
        fprintf(stderr, "tpw_stream_loopback: %d video stream(s) logging (%s, %s, %dx%d, fps=%s)\n",
                video_streams_n, use_dmabuf ? "dmabuf" : "cpu-mapped", pixel_format, width, height,
                fps > 0 ? "fixed" : "auto");
    fprintf(stderr, "tpw_stream_loopback: press Ctrl+C to stop...\n");

    while (g_running)
        sleep(1);

cleanup:
    for (int i = 0; i < video_started; i++) {
        tpw_stream_stop(video_streams[i], false);
        tpw_stream_destroy(video_streams[i]);
    }
    free(video_streams);
    free(video_ctxs);

    if (audio_playback) {
        tpw_stream_stop(audio_playback, false);
        tpw_stream_destroy(audio_playback);
    }
    if (audio_capture) {
        tpw_stream_stop(audio_capture, false);
        tpw_stream_destroy(audio_capture);
    }
    size_t dropped = ring.data ? atomic_load_explicit(&ring.dropped, memory_order_relaxed) : 0;
    if (ring.data)
        ring_destroy(&ring);
    if (dropped > 0)
        fprintf(stderr, "tpw_stream_loopback: dropped %zu bytes of audio (playback fell behind)\n", dropped);

    return status;
}
