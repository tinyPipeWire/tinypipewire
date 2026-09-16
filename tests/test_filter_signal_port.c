/* SPDX-License-Identifier: MIT */

#include <unistd.h>

#include "tpw/tpw_filter.h"
#include "tpw_test.h"

static int g_cycles = 0;
static size_t g_last_n_buffers = 0;

static void process_cb(tpw_filter_h filter, tpw_filter_port_buffer* buffers, size_t n_buffers, void* user_data)
{
    (void)filter;
    (void)buffers;
    (void)user_data;
    g_cycles++;
    g_last_n_buffers = n_buffers;
}

int main(void)
{
    tpw_filter_h filter = tpw_filter_create("tpw-test-signal", process_cb, NULL);
    TPW_ASSERT(filter != NULL);

    /* A signal port needs no config; both directions are valid. */
    tpw_filter_port_h sig_in = tpw_filter_add_signal_port(filter, TPW_FILTER_PORT_INPUT);
    TPW_ASSERT(sig_in != NULL);
    tpw_filter_port_h audio_in = tpw_filter_add_audio_port(filter, TPW_FILTER_PORT_INPUT,
                                                             &(tpw_audio_config){ .sample_rate = 48000, .channels = 2 });
    TPW_ASSERT(audio_in != NULL);

    TPW_ASSERT_EQ(tpw_filter_start(filter), TPW_OK);

    /* Pushing data into a signal input port works through the same
     * generic staging mechanism as any other port kind. */
    float samples[4] = { 0.0f, 0.25f, 0.5f, 0.75f };
    TPW_ASSERT_EQ(tpw_filter_push_port_data(filter, sig_in, samples, sizeof(samples), -1), TPW_OK);

    sleep(1);

    /* Both ports' buffers must arrive together, every cycle. */
    TPW_ASSERT(g_cycles > 0);
    TPW_ASSERT_EQ(g_last_n_buffers, (size_t)2);

    tpw_filter_stop(filter, false);
    tpw_filter_destroy(filter);

    /* Adding a signal port after the filter has started is rejected. */
    tpw_filter_h started = tpw_filter_create("tpw-test-signal-started", process_cb, NULL);
    TPW_ASSERT(started != NULL);
    TPW_ASSERT(tpw_filter_add_audio_port(started, TPW_FILTER_PORT_INPUT,
                                          &(tpw_audio_config){ .sample_rate = 48000, .channels = 2 }) != NULL);
    TPW_ASSERT_EQ(tpw_filter_start(started), TPW_OK);
    TPW_ASSERT(tpw_filter_add_signal_port(started, TPW_FILTER_PORT_OUTPUT) == NULL);
    tpw_filter_stop(started, false);
    tpw_filter_destroy(started);

    return 0;
}
