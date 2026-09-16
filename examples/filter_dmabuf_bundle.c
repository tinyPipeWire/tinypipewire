/* SPDX-License-Identifier: MIT */

#include <signal.h>
#include <stdio.h>
#include <unistd.h>

#include "tpw/tpw_filter.h"

static volatile sig_atomic_t g_running = 1;

static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

/* buffers[0] is the DMABUF video input, buffers[1] the faster signal input,
 * matching the port-adding order in main(). The camera frame is reached
 * only through the DMABUF accessor (its `data` pointer is always NULL). */
static void on_process(tpw_filter_h filter, tpw_filter_port_buffer* buffers, size_t n_buffers, void* user_data)
{
    (void)filter;
    (void)user_data;
    if (n_buffers < 2)
        return;

    tpw_filter_port_buffer* video = &buffers[0];
    tpw_filter_port_buffer* sig = &buffers[1];

    tpw_dmabuf_plane planes[4];
    size_t n_planes = tpw_filter_port_get_dmabuf_planes(video, planes, 4);

    /* With hold enabled, `fresh` distinguishes a newly captured frame from
     * a re-presented one; `seq` counts genuinely new frames. */
    if (n_planes > 0)
        printf("bundle: video fd=%d stride=%u %s seq=%llu (pts=%lld ns), signal_bytes=%zu\n",
               planes[0].fd, planes[0].stride, video->fresh ? "NEW " : "held",
               (unsigned long long)video->seq, (long long)video->pts, sig->size);
    else
        printf("bundle: video (no DMABUF frame yet), signal_bytes=%zu\n", sig->size);
}

int main(void)
{
    signal(SIGINT, on_signal);

    tpw_filter_h filter = tpw_filter_create("tpw-filter-dmabuf-bundle", on_process, NULL);
    if (!filter) {
        fprintf(stderr, "failed to create filter (is PipeWire running?)\n");
        return 1;
    }

    tpw_video_config vcfg = { .width = 640, .height = 480, .pixel_format = "RGB", .fps = 30 };
    tpw_filter_port_opts dmabuf = { .memory = TPW_PORT_MEMORY_DMABUF };
    tpw_filter_port_h video_in = tpw_filter_add_video_port_ex(filter, TPW_FILTER_PORT_INPUT, &vcfg, &dmabuf);

    tpw_filter_port_h sig_in = tpw_filter_add_signal_port(filter, TPW_FILTER_PORT_INPUT);
    if (!video_in || !sig_in) {
        fprintf(stderr, "failed to add filter ports\n");
        tpw_filter_destroy(filter);
        return 1;
    }

    /* The camera (~33 ms) is slower than this loop, so hold its last frame
     * to keep every bundle complete on the faster cycles. */
    tpw_filter_port_set_hold(video_in, true);

    /* Prefer bundling no coarser than every 10 ms (a hint; the graph still
     * picks the driver). */
    tpw_filter_set_period_hint(filter, 10000000);

    if (tpw_filter_start(filter) != TPW_OK) {
        fprintf(stderr, "failed to start filter\n");
        tpw_filter_destroy(filter);
        return 1;
    }

    printf("bundling a DMABUF camera frame with a faster signal input; link a DMABUF\n"
           "video source to the filter's video port. Press Ctrl+C to stop...\n");
    float value = 0.0f;
    while (g_running) {
        tpw_filter_push_port_data(filter, sig_in, &value, sizeof(value), -1);
        value += 0.05f;
        if (value > 1.0f)
            value -= 2.0f;
        usleep(10000); /* 10 ms: faster than the 30 fps camera */
    }

    tpw_filter_stop(filter, false);
    tpw_filter_destroy(filter);
    return 0;
}
