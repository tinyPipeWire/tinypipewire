/* SPDX-License-Identifier: MIT */

/* Needs a real camera or microphone, and exits 77 to skip without one.
 * Assertions are deliberately loose, checking presence rather than exact
 * counts, since frame timing and format negotiation vary per machine. */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <tpw/tpw_filter.h>

#include "tpw_test.h"
#include "tpw_test_hw_discover.h"

#define TEST_SKIP 77

/* How long to let the graph run before checking what arrived. */
#define RUN_USEC (1500 * 1000)

struct counters {
    unsigned cycles;
    unsigned video_buffers;
    unsigned dmabuf_frames;
    unsigned held_cycles;
    unsigned signal_buffers;
    int last_fd;
    bool seen_fresh;
};

static void on_process(tpw_filter_h filter, tpw_filter_port_buffer* buffers, size_t n_buffers, void* user_data)
{
    (void)filter;
    struct counters* c = user_data;
    c->cycles++;

    for (size_t i = 0; i < n_buffers; i++) {
        tpw_dmabuf_plane plane;
        if (tpw_filter_port_get_dmabuf_planes(&buffers[i], &plane, 1) > 0) {
            c->dmabuf_frames++;
            c->last_fd = plane.fd;
            if (buffers[i].fresh)
                c->seen_fresh = true;
            else if (c->seen_fresh)
                c->held_cycles++; /* a re-presented frame, only after a real one */
            c->video_buffers++;
        } else if (buffers[i].data && buffers[i].size > 0) {
            c->signal_buffers++;
        }
    }
}

/* Counts mapped buffers, for the shared-source test's plain ports. */
static void on_process_count(tpw_filter_h filter, tpw_filter_port_buffer* buffers, size_t n_buffers,
                              void* user_data)
{
    (void)filter;
    unsigned* count = user_data;

    for (size_t i = 0; i < n_buffers; i++) {
        if (buffers[i].data && buffers[i].size > 0)
            (*count)++;
    }
}

/* Three filters naming one source must all link and all receive. Leaving
 * the output port to the core hands each consumer a different port and
 * fails once they run out, whatever the node's port count. */
static void test_shared_source(const char* target, bool video)
{
    enum { N = 3 };
    unsigned received[N] = { 0 };
    tpw_filter_h filter[N];
    tpw_filter_port_h port[N];
    tpw_video_config cfg = { .width = 640, .height = 480, .pixel_format = "YUYV", .fps = 30 };

    for (int i = 0; i < N; i++) {
        filter[i] = tpw_filter_create("tpw-hw-shared", on_process_count, &received[i]);
        TPW_ASSERT(filter[i] != NULL);
        port[i] = video ? tpw_filter_add_video_port(filter[i], TPW_FILTER_PORT_INPUT, &cfg)
                        : tpw_filter_add_signal_port(filter[i], TPW_FILTER_PORT_INPUT);
        TPW_ASSERT(port[i] != NULL);
        TPW_ASSERT_EQ(tpw_filter_start(filter[i]), TPW_OK);
    }

    for (int i = 0; i < N; i++) {
        int res = tpw_filter_port_link(port[i], target);
        printf("  shared link %d -> '%s': %d\n", i, target, res);
        TPW_ASSERT_EQ(res, TPW_OK);
    }

    usleep(RUN_USEC);

    printf("  shared buffers: %u / %u / %u\n", received[0], received[1], received[2]);
    for (int i = 0; i < N; i++)
        TPW_ASSERT(received[i] > 0);

    for (int i = 0; i < N; i++)
        tpw_filter_destroy(filter[i]);
}

/* Links a camera to a DMABUF video port and checks frames really flow. A
 * microphone, when present, is linked to a signal port as well: audio
 * drives the graph faster than the camera, which is what engages hold. */
static void test_video_link(const char* camera, const char* mic)
{
    struct counters c = { .last_fd = -1 };
    tpw_filter_h filter = tpw_filter_create("tpw-hw-video", on_process, &c);
    TPW_ASSERT(filter != NULL);

    tpw_video_config cfg = { .width = 640, .height = 480, .pixel_format = "YUYV", .fps = 30 };
    tpw_filter_port_opts opts = { .memory = TPW_PORT_MEMORY_DMABUF };
    tpw_filter_port_h video_in = tpw_filter_add_video_port_ex(filter, TPW_FILTER_PORT_INPUT, &cfg, &opts);
    TPW_ASSERT(video_in != NULL);
    TPW_ASSERT_EQ(tpw_filter_port_set_hold(video_in, true), TPW_OK);

    tpw_filter_port_h sig_in = mic ? tpw_filter_add_signal_port(filter, TPW_FILTER_PORT_INPUT) : NULL;
    TPW_ASSERT(!mic || sig_in != NULL);
    TPW_ASSERT_EQ(tpw_filter_start(filter), TPW_OK);

    if (sig_in)
        TPW_ASSERT_EQ(tpw_filter_port_link(sig_in, mic), TPW_OK);

    int res = tpw_filter_port_link(video_in, camera);
    printf("  link video -> '%s': %d\n", camera, res);
    TPW_ASSERT_EQ(res, TPW_OK);

    /* Re-linking an already-linked port is rejected, and unlinking makes
     * the port reusable. */
    TPW_ASSERT_EQ(tpw_filter_port_link(video_in, camera), TPW_ERR_INVALID_ARG);
    TPW_ASSERT_EQ(tpw_filter_port_unlink(video_in), TPW_OK);
    TPW_ASSERT_EQ(tpw_filter_port_unlink(video_in), TPW_ERR_NOT_CONFIGURED);
    TPW_ASSERT_EQ(tpw_filter_port_link(video_in, camera), TPW_OK);

    usleep(RUN_USEC);

    printf("  cycles=%u dmabuf_frames=%u held=%u last_fd=%d\n", c.cycles, c.dmabuf_frames, c.held_cycles,
           c.last_fd);
    if (c.dmabuf_frames > 0) {
        /* A frame arrived, so it must carry a usable descriptor. */
        TPW_ASSERT(c.last_fd >= 0);
        TPW_ASSERT(c.seen_fresh);
        /* With audio driving the graph, most cycles fall between camera
         * frames and must re-present the held one. */
        if (sig_in)
            TPW_ASSERT(c.held_cycles > 0);
    } else {
        /* The link is up, but this camera and format did not settle on
         * DMABUF. A port advertising DMABUF only is strict by design. */
        printf("  note: link established but no DMABUF frames negotiated\n");
    }

    tpw_filter_stop(filter, false);
    /* stop() releases links on its own. */
    TPW_ASSERT_EQ(tpw_filter_port_unlink(video_in), TPW_ERR_NOT_CONFIGURED);
    tpw_filter_destroy(filter);
}

/* Links a microphone to a signal port (audio/dsp), which is the port kind
 * that matches an audio device's graph, and confirms it drives the filter. */
static void test_audio_link(const char* mic)
{
    struct counters c = { .last_fd = -1 };
    tpw_filter_h filter = tpw_filter_create("tpw-hw-audio", on_process, &c);
    TPW_ASSERT(filter != NULL);

    tpw_filter_port_h sig_in = tpw_filter_add_signal_port(filter, TPW_FILTER_PORT_INPUT);
    TPW_ASSERT(sig_in != NULL);
    TPW_ASSERT_EQ(tpw_filter_start(filter), TPW_OK);

    int res = tpw_filter_port_link(sig_in, mic);
    printf("  link signal -> '%s': %d\n", mic, res);
    TPW_ASSERT_EQ(res, TPW_OK);

    usleep(RUN_USEC);

    printf("  cycles=%u signal_buffers=%u\n", c.cycles, c.signal_buffers);
    /* The microphone is driving the graph, so the filter must have run. */
    TPW_ASSERT(c.cycles > 0);

    /* Draining waits for already-queued input to reach on_process too. */
    TPW_ASSERT_EQ(tpw_filter_stop(filter, true), TPW_OK);
    tpw_filter_destroy(filter);
}

/* An audio/raw port does not negotiate against a device's audio/dsp graph.
 * The memo behind this feature found that by hand; pin it down here. */
static void test_audio_raw_is_incompatible(const char* mic)
{
    struct counters c = { .last_fd = -1 };
    tpw_filter_h filter = tpw_filter_create("tpw-hw-audio-raw", on_process, &c);
    TPW_ASSERT(filter != NULL);

    tpw_audio_config cfg = { .sample_rate = 48000, .channels = 2, .format = "F32" };
    tpw_filter_port_h audio_in = tpw_filter_add_audio_port(filter, TPW_FILTER_PORT_INPUT, &cfg);
    TPW_ASSERT(audio_in != NULL);
    TPW_ASSERT_EQ(tpw_filter_start(filter), TPW_OK);

    int res = tpw_filter_port_link(audio_in, mic);
    printf("  link audio/raw -> '%s': %d (expected a clean failure)\n", mic, res);
    TPW_ASSERT(res != TPW_OK);
    /* Whatever the failure, no partial link may be left behind. */
    TPW_ASSERT_EQ(tpw_filter_port_unlink(audio_in), TPW_ERR_NOT_CONFIGURED);

    tpw_filter_stop(filter, false);
    tpw_filter_destroy(filter);
}

int main(void)
{
    char camera[256], mic[256];
    bool have_camera = tpw_test_find_node("Video/Source", camera, sizeof(camera));
    bool have_mic = tpw_test_find_node("Audio/Source", mic, sizeof(mic));

    if (!have_camera && !have_mic) {
        printf("no Video/Source or Audio/Source node present; skipping\n");
        return TEST_SKIP;
    }

    if (have_camera) {
        printf("camera: %s\n", camera);
        test_video_link(camera, have_mic ? mic : NULL);
        test_shared_source(camera, true);
    }
    if (have_mic) {
        printf("microphone: %s\n", mic);
        test_audio_link(mic);
        test_audio_raw_is_incompatible(mic);
        test_shared_source(mic, false);
    }
    return 0;
}
