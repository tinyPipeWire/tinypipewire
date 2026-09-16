/* SPDX-License-Identifier: MIT */

#include <unistd.h>

#include "tpw/tpw_filter.h"
#include "tpw_test.h"

static int g_calls = 0;
static size_t g_last_size = 0;
static int64_t g_last_pts = 0;

static void process_cb(tpw_filter_h filter, tpw_filter_port_buffer* buffers, size_t n_buffers, void* user_data)
{
    (void)filter;
    (void)user_data;
    for (size_t i = 0; i < n_buffers; i++) {
        if (buffers[i].data) {
            g_calls++;
            g_last_size = buffers[i].size;
            g_last_pts = buffers[i].pts;
        }
    }
}

int main(void)
{
    tpw_filter_h filter = tpw_filter_create("tpw-test-push", process_cb, NULL);
    TPW_ASSERT(filter != NULL);

    tpw_audio_config cfg = { .sample_rate = 48000, .channels = 2 };
    tpw_filter_port_h in_port = tpw_filter_add_audio_port(filter, TPW_FILTER_PORT_INPUT, &cfg);
    tpw_filter_port_h out_port = tpw_filter_add_audio_port(filter, TPW_FILTER_PORT_OUTPUT, &cfg);
    TPW_ASSERT(in_port != NULL);
    TPW_ASSERT(out_port != NULL);

    /* Pushing to an output port is rejected. */
    char dummy[4] = { 0 };
    TPW_ASSERT_EQ(tpw_filter_push_port_data(filter, out_port, dummy, sizeof(dummy), -1), TPW_ERR_INVALID_ARG);

    TPW_ASSERT_EQ(tpw_filter_start(filter), TPW_OK);

    /* Only the most recently pushed buffer per port is kept. */
    char first[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    char second[16] = { 0 };
    TPW_ASSERT_EQ(tpw_filter_push_port_data(filter, in_port, first, sizeof(first), 1000), TPW_OK);
    TPW_ASSERT_EQ(tpw_filter_push_port_data(filter, in_port, second, sizeof(second), 2000), TPW_OK);

    sleep(1);

    TPW_ASSERT(g_calls > 0);
    TPW_ASSERT_EQ(g_last_size, sizeof(second));
    TPW_ASSERT_EQ(g_last_pts, (int64_t)2000);

    tpw_filter_stop(filter, false);
    tpw_filter_destroy(filter);
    return 0;
}
