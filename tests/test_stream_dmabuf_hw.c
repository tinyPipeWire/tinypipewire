/* SPDX-License-Identifier: MIT */

/* Needs a real camera, and exits 77 to skip without one. Links directly to
 * the discovered camera (feature 007's tpw_stream_link()) rather than
 * relying on autoconnect, so the test always exercises the camera it found. */

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
    unsigned dmabuf_frames;
    unsigned saw_non_null_data;
    unsigned saw_pts;
    int last_fd;
    int error_code;
};

static void on_data(tpw_stream_h stream, const tpw_stream_buffer* buf, void* user_data)
{
    struct counters* c = user_data;
    c->frames++;
    if (buf->data)
        c->saw_non_null_data++;
    if (buf->pts >= 0)
        c->saw_pts++;

    tpw_dmabuf_plane plane;
    if (tpw_stream_get_dmabuf_planes(stream, &plane, 1) > 0) {
        c->dmabuf_frames++;
        c->last_fd = plane.fd;
    }
}

static void on_error(tpw_stream_h stream, int error_code, void* user_data)
{
    (void)stream;
    struct counters* c = user_data;
    c->error_code = error_code;
}

struct plane_counters {
    unsigned frames;
    unsigned max_planes;
    bool offsets_increase;
};

static void on_plane_data(tpw_stream_h stream, const tpw_stream_buffer* buf, void* user_data)
{
    (void)buf;
    struct plane_counters* c = user_data;
    c->frames++;

    tpw_dmabuf_plane planes[4];
    size_t n = tpw_stream_get_dmabuf_planes(stream, planes, 4);
    if (n > c->max_planes)
        c->max_planes = (unsigned)n;
    if (n >= 2)
        c->offsets_increase = planes[1].offset >= planes[0].offset;
}

/* Multi-plane DMABUF (NV12/I420) has no known source in this codebase's
 * test hardware; this records whether the discovered camera offers it
 * rather than requiring it, per the feature's own stated assumption. */
static void check_multiplane_format(const char* camera, const char* pixel_format, unsigned expect_planes)
{
    struct plane_counters c = { 0 };
    tpw_stream_h s = tpw_stream_create(TPW_STREAM_TYPE_VIDEO, on_plane_data, &c);
    if (!s)
        return;

    tpw_stream_set_autoconnect(s, false);
    tpw_video_config cfg = { .width = 640, .height = 480, .pixel_format = pixel_format, .fps = 30 };
    tpw_stream_dmabuf_opts opts = { .memory = TPW_PORT_MEMORY_DMABUF };
    if (tpw_stream_set_video_config_ex(s, &cfg, &opts) != TPW_OK || tpw_stream_start(s) != TPW_OK ||
        tpw_stream_link(s, camera) != TPW_OK) {
        printf("%s: camera rejected the request, skipping\n", pixel_format);
        tpw_stream_destroy(s);
        return;
    }

    usleep(RUN_USEC);
    tpw_stream_stop(s, false);
    tpw_stream_destroy(s);

    if (c.frames == 0 || c.max_planes == 0) {
        printf("%s: camera does not offer this format as DMABUF, skipping\n", pixel_format);
        return;
    }

    printf("%s: frames=%u max_planes=%u\n", pixel_format, c.frames, c.max_planes);
    TPW_ASSERT_EQ(c.max_planes, expect_planes);
    TPW_ASSERT(c.offsets_increase);
}

int main(void)
{
    char camera[256];
    if (!tpw_test_find_node("Video/Source", camera, sizeof(camera))) {
        printf("no camera present, skipping\n");
        return TEST_SKIP;
    }
    printf("capturing from: %s\n", camera);

    struct counters c = { .last_fd = -1 };
    tpw_stream_h stream = tpw_stream_create(TPW_STREAM_TYPE_VIDEO, on_data, &c);
    if (!stream) {
        printf("no PipeWire connection, skipping\n");
        return TEST_SKIP;
    }

    TPW_ASSERT_EQ(tpw_stream_set_error_cb(stream, on_error), TPW_OK);
    TPW_ASSERT_EQ(tpw_stream_set_autoconnect(stream, false), TPW_OK);

    tpw_video_config cfg = { .width = 640, .height = 480, .pixel_format = "YUYV", .fps = 30 };
    tpw_stream_dmabuf_opts opts = { .memory = TPW_PORT_MEMORY_DMABUF };
    TPW_ASSERT_EQ(tpw_stream_set_video_config_ex(stream, &cfg, &opts), TPW_OK);
    TPW_ASSERT_EQ(tpw_stream_start(stream), TPW_OK);
    TPW_ASSERT_EQ(tpw_stream_link(stream, camera), TPW_OK);

    usleep(RUN_USEC);

    TPW_ASSERT_EQ(tpw_stream_stop(stream, false), TPW_OK);
    tpw_stream_destroy(stream);

    printf("frames=%u dmabuf_frames=%u last_fd=%d pts_seen=%u\n", c.frames, c.dmabuf_frames, c.last_fd,
           c.saw_pts);

    /* Every delivered frame came in as a DMABUF plane, with no CPU-mapped
     * data ever handed out, a valid fd, and a real timestamp. */
    TPW_ASSERT(c.frames > 0);
    TPW_ASSERT_EQ(c.dmabuf_frames, c.frames);
    TPW_ASSERT_EQ(c.saw_non_null_data, 0u);
    TPW_ASSERT(c.last_fd >= 0);
    TPW_ASSERT(c.saw_pts > 0);
    TPW_ASSERT_EQ(c.error_code, 0);

    /* NV12/I420 are checked against the same camera, but only when it
     * actually offers them; a YUYV-only webcam is not a test failure. */
    check_multiplane_format(camera, "NV12", 2);
    check_multiplane_format(camera, "I420", 3);

    printf("test_stream_dmabuf_hw: passed\n");
    return 0;
}
