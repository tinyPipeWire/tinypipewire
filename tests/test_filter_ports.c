/* SPDX-License-Identifier: MIT */

#include <string.h>

#include "tpw/tpw_filter.h"
#include "tpw_filter_internal.h"
#include "tpw_test.h"

static void noop_process_cb(tpw_filter_h filter, tpw_filter_port_buffer* buffers, size_t n_buffers,
                             void* user_data)
{
    (void)filter;
    (void)buffers;
    (void)n_buffers;
    (void)user_data;
}

int main(void)
{
    /* Starting with zero ports is rejected. */
    tpw_filter_h empty = tpw_filter_create("tpw-test-empty", noop_process_cb, NULL);
    TPW_ASSERT(empty != NULL);
    TPW_ASSERT_EQ(tpw_filter_start(empty), TPW_ERR_NOT_CONFIGURED);
    tpw_filter_destroy(empty);

    tpw_filter_h filter = tpw_filter_create("tpw-test-filter", noop_process_cb, NULL);
    TPW_ASSERT(filter != NULL);
    /* The name given at creation must be the one used as the underlying
     * PipeWire node's identifying property (whitebox check). */
    TPW_ASSERT(strcmp(((struct tpw_filter*)filter)->name, "tpw-test-filter") == 0);

    /* Invalid audio/video configs are rejected (NULL handle returned). */
    tpw_audio_config bad_audio = { .sample_rate = 0, .channels = 2 };
    TPW_ASSERT(tpw_filter_add_audio_port(filter, TPW_FILTER_PORT_INPUT, &bad_audio) == NULL);

    tpw_audio_config bad_audio_format = { .sample_rate = 48000, .channels = 2, .format = "NOT_A_FORMAT" };
    TPW_ASSERT(tpw_filter_add_audio_port(filter, TPW_FILTER_PORT_INPUT, &bad_audio_format) == NULL);

    tpw_video_config bad_video = { .width = 640, .height = 480, .pixel_format = "NOT_A_FORMAT", .fps = 0 };
    TPW_ASSERT(tpw_filter_add_video_port(filter, TPW_FILTER_PORT_INPUT, &bad_video) == NULL);

    /* A filter with only input ports is valid and can start. */
    tpw_audio_config audio_cfg = { .sample_rate = 48000, .channels = 2 };
    tpw_filter_port_h in_port = tpw_filter_add_audio_port(filter, TPW_FILTER_PORT_INPUT, &audio_cfg);
    TPW_ASSERT(in_port != NULL);
    TPW_ASSERT_EQ(tpw_filter_start(filter), TPW_OK);

    /* Adding a port after the filter has started is rejected. */
    TPW_ASSERT(tpw_filter_add_audio_port(filter, TPW_FILTER_PORT_OUTPUT, &audio_cfg) == NULL);

    tpw_filter_stop(filter, false);
    tpw_filter_destroy(filter);

    /* A filter with only output ports is also valid and can start. */
    tpw_filter_h out_only = tpw_filter_create("tpw-test-out-only", noop_process_cb, NULL);
    TPW_ASSERT(out_only != NULL);
    tpw_video_config video_cfg = { .width = 640, .height = 480, .pixel_format = "RGB", .fps = 30 };
    TPW_ASSERT(tpw_filter_add_video_port(out_only, TPW_FILTER_PORT_OUTPUT, &video_cfg) != NULL);

    /* MJPEG is accepted on a filter video port the same way a raw format is. */
    tpw_video_config mjpg_cfg = { .width = 640, .height = 480, .pixel_format = "MJPG", .fps = 30 };
    TPW_ASSERT(tpw_filter_add_video_port(out_only, TPW_FILTER_PORT_OUTPUT, &mjpg_cfg) != NULL);

    TPW_ASSERT_EQ(tpw_filter_start(out_only), TPW_OK);
    tpw_filter_destroy(out_only);

    return 0;
}
