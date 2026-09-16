/* SPDX-License-Identifier: MIT */

/* Needs a real camera and/or microphone, and exits 77 to skip if neither is
 * present. Captures each with plain (non-DMABUF) buffers and checks the
 * delivered data itself — the gap test_stream_dmabuf_hw.c (DMABUF-only) and
 * test_stream_lifecycle.c (discards buf) both leave uncovered. Audio and
 * video are two independent tpw_stream_h handles, since one handle is fixed
 * to a single type at creation. */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <unistd.h>

#include <tpw/tpw_stream.h>

#include "tpw_test.h"
#include "tpw_test_hw_discover.h"

#define TEST_SKIP 77
#define RUN_USEC (1500 * 1000)

struct counters {
    unsigned frames;
    unsigned saw_non_null_data;
    unsigned saw_zero_size;
    unsigned saw_pts;
    int error_code;
};

static void on_data(tpw_stream_h stream, const tpw_stream_buffer* buf, void* user_data)
{
    (void)stream;
    struct counters* c = user_data;
    c->frames++;
    if (buf->data)
        c->saw_non_null_data++;
    if (buf->data && buf->size == 0)
        c->saw_zero_size++;
    if (buf->pts >= 0)
        c->saw_pts++;
}

static void on_error(tpw_stream_h stream, int error_code, void* user_data)
{
    (void)stream;
    struct counters* c = user_data;
    c->error_code = error_code;
}

/* Captures RUN_USEC of raw video from `camera` and checks every delivered
 * frame carried real, CPU-mapped pixel data. Video pts is reliable since
 * PR #15 (SPA_META_Header requested on connect), so it is checked too. */
static void run_video_capture(const char* camera)
{
    struct counters c = { 0 };
    tpw_stream_h s = tpw_stream_create(TPW_STREAM_TYPE_VIDEO, on_data, &c);
    TPW_ASSERT(s != NULL);
    TPW_ASSERT_EQ(tpw_stream_set_error_cb(s, on_error), TPW_OK);
    TPW_ASSERT_EQ(tpw_stream_set_autoconnect(s, false), TPW_OK);

    tpw_video_config cfg = { .width = 640, .height = 480, .pixel_format = "YUYV", .fps = 30 };
    TPW_ASSERT_EQ(tpw_stream_set_video_config(s, &cfg), TPW_OK);
    TPW_ASSERT_EQ(tpw_stream_start(s), TPW_OK);
    TPW_ASSERT_EQ(tpw_stream_link(s, camera), TPW_OK);

    usleep(RUN_USEC);
    TPW_ASSERT_EQ(tpw_stream_stop(s, false), TPW_OK);
    tpw_stream_destroy(s);

    printf("video: frames=%u non_null_data=%u pts_seen=%u\n", c.frames, c.saw_non_null_data, c.saw_pts);

    TPW_ASSERT(c.frames > 0);
    TPW_ASSERT_EQ(c.saw_non_null_data, c.frames);
    TPW_ASSERT_EQ(c.saw_zero_size, 0u);
    TPW_ASSERT(c.saw_pts > 0);
    TPW_ASSERT_EQ(c.error_code, 0);
}

/* Captures RUN_USEC of raw audio from `mic`. Requests mono: tpw_stream_link()
 * rejects a target with fewer channels than requested, and a capture-only
 * mic is commonly mono (surplus channels on a stereo device are simply left
 * unconnected, per tpw_stream_link()'s docs). pts is not asserted: PipeWire's
 * audioconvert/audioadapter often drops the capture timestamp when the
 * requested format isn't the device's native one — a known upstream gap,
 * not something this test can control. */
static void run_audio_capture(const char* mic)
{
    struct counters c = { 0 };
    tpw_stream_h s = tpw_stream_create(TPW_STREAM_TYPE_AUDIO, on_data, &c);
    TPW_ASSERT(s != NULL);
    TPW_ASSERT_EQ(tpw_stream_set_error_cb(s, on_error), TPW_OK);
    TPW_ASSERT_EQ(tpw_stream_set_autoconnect(s, false), TPW_OK);

    tpw_audio_config cfg = { .sample_rate = 48000, .channels = 1 };
    TPW_ASSERT_EQ(tpw_stream_set_audio_config(s, &cfg), TPW_OK);
    TPW_ASSERT_EQ(tpw_stream_start(s), TPW_OK);
    TPW_ASSERT_EQ(tpw_stream_link(s, mic), TPW_OK);

    usleep(RUN_USEC);
    /* Draining a capture stream waits for already-queued frames to reach
     * this callback too, not just a playback stream's device. */
    TPW_ASSERT_EQ(tpw_stream_stop(s, true), TPW_OK);
    tpw_stream_destroy(s);

    printf("audio: frames=%u non_null_data=%u pts_seen=%u\n", c.frames, c.saw_non_null_data, c.saw_pts);

    TPW_ASSERT(c.frames > 0);
    TPW_ASSERT_EQ(c.saw_non_null_data, c.frames);
    TPW_ASSERT_EQ(c.saw_zero_size, 0u);
    TPW_ASSERT_EQ(c.error_code, 0);
}

struct mjpg_counters {
    unsigned frames;
    unsigned saw_non_null_data;
    unsigned saw_zero_size;
    size_t first_size;
    bool saw_different_size;
    int error_code;
};

static void on_mjpg_data(tpw_stream_h stream, const tpw_stream_buffer* buf, void* user_data)
{
    (void)stream;
    struct mjpg_counters* c = user_data;
    c->frames++;
    if (buf->data)
        c->saw_non_null_data++;
    if (buf->data && buf->size == 0)
        c->saw_zero_size++;
    if (c->frames == 1)
        c->first_size = buf->size;
    else if (buf->size != c->first_size)
        c->saw_different_size = true;
}

static void on_mjpg_error(tpw_stream_h stream, int error_code, void* user_data)
{
    (void)stream;
    struct mjpg_counters* c = user_data;
    c->error_code = error_code;
}

/* Not every camera offers MJPEG at this size, so a rejected link is
 * printed and skipped rather than treated as a failure. */
static void run_mjpg_capture_if_supported(const char* camera)
{
    struct mjpg_counters c = { 0 };
    tpw_stream_h s = tpw_stream_create(TPW_STREAM_TYPE_VIDEO, on_mjpg_data, &c);
    TPW_ASSERT(s != NULL);
    TPW_ASSERT_EQ(tpw_stream_set_error_cb(s, on_mjpg_error), TPW_OK);
    TPW_ASSERT_EQ(tpw_stream_set_autoconnect(s, false), TPW_OK);

    tpw_video_config cfg = { .width = 1280, .height = 720, .pixel_format = "MJPG", .fps = 30 };
    TPW_ASSERT_EQ(tpw_stream_set_video_config(s, &cfg), TPW_OK);
    TPW_ASSERT_EQ(tpw_stream_start(s), TPW_OK);

    if (tpw_stream_link(s, camera) != TPW_OK) {
        printf("MJPG: camera rejected the request, skipping\n");
        tpw_stream_destroy(s);
        return;
    }

    usleep(RUN_USEC);
    TPW_ASSERT_EQ(tpw_stream_stop(s, false), TPW_OK);
    tpw_stream_destroy(s);

    printf("MJPG: frames=%u non_null_data=%u differing_sizes=%s\n", c.frames, c.saw_non_null_data,
           c.saw_different_size ? "yes" : "no");

    if (c.frames == 0) {
        printf("MJPG: no frames delivered, skipping\n");
        return;
    }

    TPW_ASSERT_EQ(c.saw_non_null_data, c.frames);
    TPW_ASSERT_EQ(c.saw_zero_size, 0u);
    TPW_ASSERT(c.saw_different_size);
    TPW_ASSERT_EQ(c.error_code, 0);
}

int main(void)
{
    char camera[256];
    char mic[256];
    bool have_camera = tpw_test_find_node("Video/Source", camera, sizeof(camera));
    bool have_mic = tpw_test_find_node("Audio/Source", mic, sizeof(mic));

    if (!have_camera && !have_mic) {
        printf("no camera or microphone present, skipping\n");
        return TEST_SKIP;
    }

    if (have_camera) {
        printf("capturing video from: %s\n", camera);
        run_video_capture(camera);
        run_mjpg_capture_if_supported(camera);
    } else {
        printf("no camera present, skipping video capture\n");
    }

    if (have_mic) {
        printf("capturing audio from: %s\n", mic);
        run_audio_capture(mic);
    } else {
        printf("no microphone present, skipping audio capture\n");
    }

    printf("test_stream_capture_hw: passed\n");
    return 0;
}
