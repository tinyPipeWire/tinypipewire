/* SPDX-License-Identifier: MIT */

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include "tpw/tpw_filter.h"

static volatile sig_atomic_t g_running = 1;

static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

static void on_error(tpw_filter_h filter, tpw_filter_port_h port, int error_code, void* user_data)
{
    (void)filter;
    (void)port;
    (void)user_data;
    fprintf(stderr, "filter_mix: a port's source was lost (error %d)\n", error_code);
}

/* Sums the two audio inputs sample-by-sample (with clipping) into the
 * single audio output; buffers[0]/[1] are inputs, buffers[2] is output,
 * matching the port-adding order in main(). */
static void on_process(tpw_filter_h filter, tpw_filter_port_buffer* buffers, size_t n_buffers, void* user_data)
{
    (void)filter;
    (void)user_data;
    if (n_buffers < 3)
        return;

    tpw_filter_port_buffer* in0 = &buffers[0];
    tpw_filter_port_buffer* in1 = &buffers[1];
    tpw_filter_port_buffer* out = &buffers[2];

    printf("filter_mix: in0=%zu bytes in1=%zu bytes\n", in0->size, in1->size);

    if (!out->data || out->capacity == 0 || (!in0->data && !in1->data))
        return;

    size_t n = out->capacity;
    if (in0->data && in0->size < n)
        n = in0->size;
    if (in1->data && in1->size < n)
        n = in1->size;

    size_t n_samples = n / sizeof(int16_t);
    int16_t* o = out->data;
    const int16_t* a = in0->data;
    const int16_t* b = in1->data;
    for (size_t i = 0; i < n_samples; i++) {
        int32_t sum = 0;
        if (a)
            sum += a[i];
        if (b)
            sum += b[i];
        if (sum > INT16_MAX)
            sum = INT16_MAX;
        if (sum < INT16_MIN)
            sum = INT16_MIN;
        o[i] = (int16_t)sum;
    }
    out->size = n_samples * sizeof(int16_t);
}

int main(void)
{
    signal(SIGINT, on_signal);

    tpw_filter_h filter = tpw_filter_create("tpw-filter-mix", on_process, NULL);
    if (!filter) {
        fprintf(stderr, "failed to create filter (is PipeWire running?)\n");
        return 1;
    }

    tpw_filter_set_error_cb(filter, on_error);

    tpw_audio_config cfg = { .sample_rate = 48000, .channels = 2 };
    tpw_filter_port_h in0 = tpw_filter_add_audio_port(filter, TPW_FILTER_PORT_INPUT, &cfg);
    tpw_filter_port_h in1 = tpw_filter_add_audio_port(filter, TPW_FILTER_PORT_INPUT, &cfg);
    tpw_filter_port_h out = tpw_filter_add_audio_port(filter, TPW_FILTER_PORT_OUTPUT, &cfg);
    if (!in0 || !in1 || !out) {
        fprintf(stderr, "failed to add filter ports\n");
        tpw_filter_destroy(filter);
        return 1;
    }

    if (tpw_filter_start(filter) != TPW_OK) {
        fprintf(stderr, "failed to start filter\n");
        tpw_filter_destroy(filter);
        return 1;
    }

    printf("mixing two audio inputs into one output, press Ctrl+C to stop...\n");
    while (g_running)
        sleep(1);

    tpw_filter_stop(filter, false);
    tpw_filter_destroy(filter);
    return 0;
}
