/* SPDX-License-Identifier: MIT */

/* Needs a real camera, exiting 77 to skip without one. Checks the promise the
 * query makes: every entry is one tpw_stream_set_video_config() accepts. */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <tpw/tpw_stream.h>

#include "tpw_test.h"
#include "tpw_test_hw_discover.h"

#define TEST_SKIP 77
#define MAX_FORMATS 64

/* The pixel format names tpw_video_config documents; nothing else may
 * reach a caller, since nothing else could be passed back. */
static bool is_known_pixel_format(const char* name)
{
    static const char* known[] = { "RGB", "YUYV", "NV12", "NV21", "I420", "MJPG", "H264" };
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        if (strcmp(known[i], name) == 0)
            return true;
    }
    return false;
}

static void ignore_data_cb(tpw_stream_h stream, const tpw_stream_buffer* buf, void* user_data)
{
    (void)stream;
    (void)buf;
    (void)user_data;
}

int main(void)
{
    char node[256];
    if (!tpw_test_find_node("Video/Source", node, sizeof(node))) {
        printf("no camera in the graph; skipping\n");
        return TEST_SKIP;
    }

    tpw_stream_h s = tpw_stream_create(TPW_STREAM_TYPE_VIDEO, ignore_data_cb, NULL);
    TPW_ASSERT(s != NULL);

    tpw_video_format_info fmts[MAX_FORMATS];
    size_t n = 0;
    int res = tpw_stream_get_target_video_formats(s, node, fmts, MAX_FORMATS, &n);
    if (res != TPW_OK) {
        printf("'%s' could not be read (error %d; in use elsewhere?); skipping\n", node, res);
        tpw_stream_destroy(s);
        return TEST_SKIP;
    }
    if (n == 0) {
        printf("'%s' names no format this library knows; skipping\n", node);
        tpw_stream_destroy(s);
        return TEST_SKIP;
    }
    printf("'%s' offers %zu format(s)\n", node, n);

    size_t stored = n < MAX_FORMATS ? n : MAX_FORMATS;
    for (size_t i = 0; i < stored; i++) {
        const tpw_video_format_info* f = &fmts[i];

        /* Entries must be self-consistent and named, or a caller copying one
         * into a tpw_video_config would be passing on nonsense. */
        TPW_ASSERT(is_known_pixel_format(f->pixel_format));
        TPW_ASSERT(f->width > 0 && f->height > 0);
        TPW_ASSERT(f->width_max >= f->width);
        TPW_ASSERT(f->height_max >= f->height);

        /* n_fps is what `fps` actually holds, so this needs no clamping. */
        TPW_ASSERT(f->n_fps <= sizeof(f->fps) / sizeof(f->fps[0]));
        for (size_t r = 0; r < f->n_fps; r++) {
            TPW_ASSERT(f->fps[r] > 0); /* 0 would read as "negotiate automatically" */
            if (r > 0)
                TPW_ASSERT(f->fps[r] < f->fps[r - 1]); /* highest first, no repeats */
        }

        printf("  %s %dx%d", f->pixel_format, f->width, f->height);
        if (f->width_max != f->width || f->height_max != f->height)
            printf("..%dx%d", f->width_max, f->height_max);
        for (size_t r = 0; r < f->n_fps; r++)
            printf("%s%d", r == 0 ? " @" : ",", f->fps[r]);
        printf("\n");
    }

    /* The contract itself: hand the first entry back unexamined and the stream
     * must configure. A range's smallest size is offered, always inside it. */
    const tpw_video_format_info* pick = &fmts[0];
    tpw_video_config cfg = {
        .width = pick->width,
        .height = pick->height,
        .pixel_format = pick->pixel_format,
        .fps = pick->n_fps > 0 ? pick->fps[0] : 0,
    };
    TPW_ASSERT_EQ(tpw_stream_set_target(s, node), TPW_OK);
    TPW_ASSERT_EQ(tpw_stream_set_video_config(s, &cfg), TPW_OK);

    tpw_stream_destroy(s);

    /* Asking again through a fresh handle, with the target set beforehand
     * and no explicit target passed, must reach the same device. */
    tpw_stream_h s2 = tpw_stream_create(TPW_STREAM_TYPE_VIDEO, ignore_data_cb, NULL);
    TPW_ASSERT(s2 != NULL);
    TPW_ASSERT_EQ(tpw_stream_set_target(s2, node), TPW_OK);
    size_t again = 0;
    TPW_ASSERT_EQ(tpw_stream_get_target_video_formats(s2, NULL, fmts, MAX_FORMATS, &again),
                  TPW_OK);
    TPW_ASSERT_EQ(again, n);

    /* A too-small buffer still reports the true count, so a caller can size
     * an array and ask again. */
    tpw_video_format_info one;
    TPW_ASSERT_EQ(tpw_stream_get_target_video_formats(s2, NULL, &one, 1, &again), TPW_OK);
    TPW_ASSERT_EQ(again, n);
    TPW_ASSERT(one.width > 0);

    /* Counting with no buffer at all is allowed and must agree. */
    TPW_ASSERT_EQ(tpw_stream_get_target_video_formats(s2, NULL, NULL, 0, &again), TPW_OK);
    TPW_ASSERT_EQ(again, n);

    tpw_stream_destroy(s2);
    return 0;
}
