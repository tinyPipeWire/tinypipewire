/* SPDX-License-Identifier: MIT */

/* Covers the video format queries without a camera: the rejections a caller
 * can hit, and the frame-rate ordering (reached via the internal header). */

#include <stddef.h>

#include "tpw/tpw_filter.h"
#include "tpw/tpw_stream.h"
#include "tpw_pw_core_internal.h"
#include "tpw_test.h"

static void ignore_data_cb(tpw_stream_h stream, const tpw_stream_buffer* buf, void* user_data)
{
    (void)stream;
    (void)buf;
    (void)user_data;
}

static void ignore_process_cb(tpw_filter_h filter, tpw_filter_port_buffer* buffers, size_t n_buffers,
                               void* user_data)
{
    (void)filter;
    (void)buffers;
    (void)n_buffers;
    (void)user_data;
}

static void test_framerate_order(void)
{
    tpw_video_format_info info = { 0 };

    /* Rates arrive in whatever order the device lists them and come back
     * highest first, which is what makes fps[0] the one to reach for. */
    tpw_video_insert_framerate(&info, 15);
    tpw_video_insert_framerate(&info, 30);
    tpw_video_insert_framerate(&info, 5);
    TPW_ASSERT_EQ(info.n_fps, (size_t)3);
    TPW_ASSERT_EQ(info.fps[0], 30);
    TPW_ASSERT_EQ(info.fps[1], 15);
    TPW_ASSERT_EQ(info.fps[2], 5);

    /* A repeat is dropped rather than stored twice: a range whose ends
     * meet reports the same rate as both min and max. */
    tpw_video_insert_framerate(&info, 15);
    TPW_ASSERT_EQ(info.n_fps, (size_t)3);

    /* Filling the array exactly keeps every rate. */
    tpw_video_format_info full = { 0 };
    const size_t cap = sizeof(full.fps) / sizeof(full.fps[0]);
    for (size_t i = 1; i <= cap; i++)
        tpw_video_insert_framerate(&full, (int)i);
    TPW_ASSERT_EQ(full.n_fps, cap);
    TPW_ASSERT_EQ(full.fps[0], (int)cap);
    TPW_ASSERT_EQ(full.fps[cap - 1], 1);

    /* Past that the slowest rate falls off and n_fps stays at the array's
     * size, so iterating to n_fps never reads beyond it. */
    tpw_video_insert_framerate(&full, 240);
    TPW_ASSERT_EQ(full.n_fps, cap);
    TPW_ASSERT_EQ(full.fps[0], 240);
    TPW_ASSERT_EQ(full.fps[1], (int)cap);
    TPW_ASSERT_EQ(full.fps[cap - 1], 2);

    /* A rate slower than everything stored is dropped, changing nothing. */
    tpw_video_insert_framerate(&full, 1);
    TPW_ASSERT_EQ(full.n_fps, cap);
    TPW_ASSERT_EQ(full.fps[0], 240);
    TPW_ASSERT_EQ(full.fps[cap - 1], 2);
}

static void test_stream_rejections(void)
{
    tpw_video_format_info fmts[4];
    size_t n = 99;

    /* No handle, no connection to ask over. */
    TPW_ASSERT_EQ(tpw_stream_get_target_video_formats(NULL, "some-node", fmts, 4, &n),
                  TPW_ERR_INVALID_ARG);
    TPW_ASSERT_EQ(n, (size_t)0);

    /* An audio stream is refused, not answered with an empty list, which
     * would instead read as "this camera offers nothing". */
    tpw_stream_h audio = tpw_stream_create(TPW_DATA_AUDIO, ignore_data_cb, NULL);
    TPW_ASSERT(audio != NULL);
    TPW_ASSERT_EQ(tpw_stream_get_target_video_formats(audio, "some-node", fmts, 4, &n),
                  TPW_ERR_INVALID_ARG);
    tpw_stream_destroy(audio);

    tpw_stream_h video = tpw_stream_create(TPW_DATA_VIDEO, ignore_data_cb, NULL);
    TPW_ASSERT(video != NULL);

    /* There is nowhere to report a count to. */
    TPW_ASSERT_EQ(tpw_stream_get_target_video_formats(video, "some-node", fmts, 4, NULL),
                  TPW_ERR_INVALID_ARG);

    /* NULL target with none set means there is nothing to ask about. */
    TPW_ASSERT_EQ(tpw_stream_get_target_video_formats(video, NULL, fmts, 4, &n),
                  TPW_ERR_INVALID_ARG);

    /* A name no node carries resolves to nothing. */
    TPW_ASSERT_EQ(
        tpw_stream_get_target_video_formats(video, "tpw-test-nonexistent-node", fmts, 4, &n),
        TPW_ERR_NOT_FOUND);

    /* NULL target falls back to the one already set, so an unresolvable
     * one still fails rather than picking some other device. */
    TPW_ASSERT_EQ(tpw_stream_set_target(video, "tpw-test-nonexistent-node"), TPW_OK);
    TPW_ASSERT_EQ(tpw_stream_get_target_video_formats(video, NULL, fmts, 4, &n),
                  TPW_ERR_NOT_FOUND);

    tpw_stream_destroy(video);
}

static void test_filter_rejections(void)
{
    tpw_video_format_info fmts[4];
    size_t n = 99;

    TPW_ASSERT_EQ(tpw_filter_get_target_video_formats(NULL, "some-node", fmts, 4, &n),
                  TPW_ERR_INVALID_ARG);
    TPW_ASSERT_EQ(n, (size_t)0);

    tpw_filter_h filter = tpw_filter_create("tpw-test-video-enum", ignore_process_cb, NULL);
    TPW_ASSERT(filter != NULL);

    TPW_ASSERT_EQ(tpw_filter_get_target_video_formats(filter, "some-node", fmts, 4, NULL),
                  TPW_ERR_INVALID_ARG);

    /* The filter takes no NULL-target shorthand: it has no target of its
     * own until a port is linked, which happens after the format is fixed. */
    TPW_ASSERT_EQ(tpw_filter_get_target_video_formats(filter, NULL, fmts, 4, &n),
                  TPW_ERR_INVALID_ARG);
    TPW_ASSERT_EQ(
        tpw_filter_get_target_video_formats(filter, "tpw-test-nonexistent-node", fmts, 4, &n),
        TPW_ERR_NOT_FOUND);

    /* "node:port" is accepted; the node half is what has to resolve. */
    TPW_ASSERT_EQ(tpw_filter_get_target_video_formats(filter, "tpw-test-nonexistent-node:capture_0",
                                                       fmts, 4, &n),
                  TPW_ERR_NOT_FOUND);

    tpw_filter_destroy(filter);
}

int main(void)
{
    test_framerate_order();
    test_stream_rejections();
    test_filter_rejections();
    return 0;
}
