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

static const char* kind_name(tpw_event_kind kind)
{
    switch (kind) {
    case TPW_EVENT_MIDI:
        return "midi";
    case TPW_EVENT_OSC:
        return "osc";
    case TPW_EVENT_PROPERTY:
        return "property";
    default:
        return "unknown";
    }
}

/* Echoes every event received on the input event port back out through
 * the output event port, one process cycle at a time. Never touches a
 * spa_pod or any other SPA/PipeWire type directly. */
static void on_process(tpw_filter_h filter, tpw_filter_port_buffer* buffers, size_t n_buffers, void* user_data)
{
    (void)filter;
    (void)user_data;
    if (n_buffers < 2)
        return;

    tpw_filter_port_h in = buffers[0].port;
    tpw_filter_port_h out = buffers[1].port;

    size_t count = tpw_filter_port_get_event_count(in);
    for (size_t i = 0; i < count; i++) {
        tpw_event ev;
        if (tpw_filter_port_get_event(in, i, &ev) != TPW_OK)
            continue;
        printf("filter_event_port: kind=%s offset=%u size=%zu\n", kind_name(ev.kind), ev.offset, ev.size);
        tpw_filter_port_push_event(out, &ev);
    }
}

int main(void)
{
    signal(SIGINT, on_signal);

    tpw_filter_h filter = tpw_filter_create("tpw-filter-event-port", on_process, NULL);
    if (!filter) {
        fprintf(stderr, "failed to create filter (is PipeWire running?)\n");
        return 1;
    }

    tpw_filter_port_h in = tpw_filter_add_event_port(filter, TPW_FILTER_PORT_INPUT);
    tpw_filter_port_h out = tpw_filter_add_event_port(filter, TPW_FILTER_PORT_OUTPUT);
    if (!in || !out) {
        fprintf(stderr, "failed to add filter ports\n");
        tpw_filter_destroy(filter);
        return 1;
    }

    printf("in port kind=%d, out port kind=%d (TPW_DATA_EVENT=%d)\n", tpw_filter_port_get_type(in),
           tpw_filter_port_get_type(out), TPW_DATA_EVENT);

    if (tpw_filter_start(filter) != TPW_OK) {
        fprintf(stderr, "failed to start filter\n");
        tpw_filter_destroy(filter);
        return 1;
    }

    printf("echoing events from the input port to the output port, press Ctrl+C to stop...\n");
    while (g_running)
        sleep(1);

    tpw_filter_stop(filter, false);
    tpw_filter_destroy(filter);
    return 0;
}
