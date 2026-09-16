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

/* Feeds a synthetic ramp value into the signal port in place of a real
 * sensor; buffers[0] is the signal input, buffers[1] is the audio input,
 * matching the port-adding order in main(). */
static void on_process(tpw_filter_h filter, tpw_filter_port_buffer* buffers, size_t n_buffers, void* user_data)
{
    (void)filter;
    (void)user_data;
    if (n_buffers < 2)
        return;

    tpw_filter_port_buffer* signal = &buffers[0];
    tpw_filter_port_buffer* audio = &buffers[1];

    size_t n_values = signal->data ? signal->size / sizeof(float) : 0;
    printf("filter_signal_port: signal_frames=%zu (pts=%lld ns) audio_bytes=%zu\n", n_values,
           (long long)signal->pts, audio->size);
}

int main(void)
{
    signal(SIGINT, on_signal);

    tpw_filter_h filter = tpw_filter_create("tpw-filter-signal-port", on_process, NULL);
    if (!filter) {
        fprintf(stderr, "failed to create filter (is PipeWire running?)\n");
        return 1;
    }

    tpw_filter_port_h sig_in = tpw_filter_add_signal_port(filter, TPW_FILTER_PORT_INPUT);
    tpw_audio_config cfg = { .sample_rate = 48000, .channels = 2 };
    tpw_filter_port_h audio_in = tpw_filter_add_audio_port(filter, TPW_FILTER_PORT_INPUT, &cfg);
    if (!sig_in || !audio_in) {
        fprintf(stderr, "failed to add filter ports\n");
        tpw_filter_destroy(filter);
        return 1;
    }

    if (tpw_filter_start(filter) != TPW_OK) {
        fprintf(stderr, "failed to start filter\n");
        tpw_filter_destroy(filter);
        return 1;
    }

    printf("feeding a synthetic signal alongside an audio input, press Ctrl+C to stop...\n");
    float value = 0.0f;
    while (g_running) {
        /* Stand in for a real sensor: a ramp value, wrapping at 1.0. No
         * hardware timestamp exists for a synthetic value like this. */
        tpw_filter_push_port_data(filter, sig_in, &value, sizeof(value), -1);
        value += 0.05f;
        if (value > 1.0f)
            value -= 2.0f;
        usleep(20000);
    }

    tpw_filter_stop(filter, false);
    tpw_filter_destroy(filter);
    return 0;
}
