/* SPDX-License-Identifier: MIT */

#include <signal.h>
#include <stdio.h>
#include <unistd.h>

#include "tpw/tpw_stream.h"

static volatile sig_atomic_t g_running = 1;

static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

static void on_data(tpw_stream_h stream, const tpw_stream_buffer* buf, void* user_data)
{
    (void)user_data;

    tpw_dmabuf_plane planes[4];
    size_t n_planes = tpw_stream_get_dmabuf_planes(stream, planes, 4);
    if (n_planes == 0) {
        printf("video: frame with no DMABUF plane (pts=%lld ns)\n", (long long)buf->pts);
        return;
    }

    printf("video: %zu plane(s), fd=%d stride=%u size=%u (pts=%lld ns)\n", n_planes, planes[0].fd,
           planes[0].stride, planes[0].size, (long long)buf->pts);
}

static void on_error(tpw_stream_h stream, int error_code, void* user_data)
{
    (void)stream;
    (void)user_data;
    fprintf(stderr, "video: source lost or could not negotiate DMABUF (error %d)\n", error_code);
    g_running = 0;
}

int main(void)
{
    signal(SIGINT, on_signal);

    tpw_stream_h stream = tpw_stream_create(TPW_STREAM_TYPE_VIDEO, on_data, NULL);
    if (!stream) {
        fprintf(stderr, "failed to create video stream (is PipeWire running?)\n");
        return 1;
    }

    tpw_stream_set_error_cb(stream, on_error);

    tpw_video_config cfg = { .width = 640, .height = 480, .pixel_format = "YUYV", .fps = 30 };
    tpw_stream_dmabuf_opts opts = { .memory = TPW_PORT_MEMORY_DMABUF };
    if (tpw_stream_set_video_config_ex(stream, &cfg, &opts) != TPW_OK) {
        fprintf(stderr, "failed to request DMABUF video format\n");
        tpw_stream_destroy(stream);
        return 1;
    }

    if (tpw_stream_start(stream) != TPW_OK) {
        fprintf(stderr, "failed to start video stream\n");
        tpw_stream_destroy(stream);
        return 1;
    }

    printf("capturing video via DMABUF, press Ctrl+C to stop...\n");
    while (g_running)
        sleep(1);

    tpw_stream_stop(stream, false);
    tpw_stream_destroy(stream);
    return 0;
}
