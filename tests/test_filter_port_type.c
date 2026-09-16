/* SPDX-License-Identifier: MIT */

#include "tpw/tpw_filter.h"
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
    tpw_filter_h filter = tpw_filter_create("tpw-test-port-type", noop_process_cb, NULL);
    TPW_ASSERT(filter != NULL);

    tpw_filter_port_h audio = tpw_filter_add_audio_port(filter, TPW_FILTER_PORT_INPUT,
                                                          &(tpw_audio_config){ .sample_rate = 48000, .channels = 2 });
    tpw_filter_port_h video = tpw_filter_add_video_port(
        filter, TPW_FILTER_PORT_INPUT, &(tpw_video_config){ .width = 640, .height = 480, .pixel_format = "RGB", .fps = 30 });
    tpw_filter_port_h signal = tpw_filter_add_signal_port(filter, TPW_FILTER_PORT_INPUT);
    tpw_filter_port_h event = tpw_filter_add_event_port(filter, TPW_FILTER_PORT_INPUT);

    TPW_ASSERT(audio != NULL);
    TPW_ASSERT(video != NULL);
    TPW_ASSERT(signal != NULL);
    TPW_ASSERT(event != NULL);

    TPW_ASSERT_EQ(tpw_filter_port_get_type(audio), TPW_DATA_AUDIO);
    TPW_ASSERT_EQ(tpw_filter_port_get_type(video), TPW_DATA_VIDEO);
    TPW_ASSERT_EQ(tpw_filter_port_get_type(signal), TPW_DATA_SIGNAL);
    TPW_ASSERT_EQ(tpw_filter_port_get_type(event), TPW_DATA_EVENT);

    tpw_filter_destroy(filter);
    return 0;
}
