/* SPDX-License-Identifier: MIT */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tpw/tpw_filter.h"
#include "tpw_filter_internal.h"
#include "tpw_test.h"

static int g_cycles = 0;
static size_t g_last_event_count = 0;
static uint32_t g_last_offset = 0;
static tpw_event_kind g_last_kind = TPW_EVENT_UNKNOWN;
static uint8_t g_last_data[64];
static size_t g_last_size = 0;

static int g_output_push_calls = 0;
static int g_output_push_unexpected = 0;

static void process_cb(tpw_filter_h filter, tpw_filter_port_buffer* buffers, size_t n_buffers, void* user_data)
{
    (void)filter;
    (void)user_data;
    g_cycles++;
    if (n_buffers < 1)
        return;

    /* Port 1 (added second) is the output event port. Unlinked, it
     * never gets a real dequeue-able buffer (confirmed in CI), so only
     * {OK, INVALID_ARG} are valid outcomes here — never anything else. */
    if (n_buffers >= 2) {
        uint8_t note_on[3] = { 0x90, 0x3c, 0x64 };
        tpw_event out_ev = { .offset = 0, .kind = TPW_EVENT_MIDI, .key = NULL, .data = note_on,
                              .size = sizeof(note_on) };
        g_output_push_calls++;
        int res = tpw_filter_port_push_event(buffers[1].port, &out_ev);
        if (res != TPW_OK && res != TPW_ERR_INVALID_ARG)
            g_output_push_unexpected++;
    }

    tpw_filter_port_h in = buffers[0].port;
    size_t count = tpw_filter_port_get_event_count(in);
    if (count == 0)
        return;
    /* Only overwrite the "last observed" globals on a cycle that
     * actually had an event — the many empty cycles before/after must
     * not erase what a prior cycle already recorded. */
    g_last_event_count = count;

    tpw_event ev;
    if (tpw_filter_port_get_event(in, 0, &ev) != TPW_OK)
        return;
    g_last_offset = ev.offset;
    g_last_kind = ev.kind;
    g_last_size = ev.size < sizeof(g_last_data) ? ev.size : sizeof(g_last_data);
    if (ev.data && g_last_size > 0)
        memcpy(g_last_data, ev.data, g_last_size);
}

/* Whitebox helper: stages one pending event on a scratch port, the way
 * tpw_filter_port_push_event() would, without needing a real filter
 * (tpw_filter_event_finish_output()/tpw_filter_event_decode() touch
 * only the port struct's own arrays, never port->filter). */
static void stage_pending(struct tpw_filter_port* port, tpw_event_kind kind, const char* key, const void* data,
                           size_t size, uint32_t offset)
{
    struct tpw_filter_pending_event* grown =
        realloc(port->pending_events, (port->n_pending_events + 1) * sizeof(*grown));
    port->pending_events = grown;
    struct tpw_filter_pending_event* dst = &port->pending_events[port->n_pending_events];
    dst->offset = offset;
    dst->kind = kind;
    dst->key = key;
    dst->size = size;
    dst->data = NULL;
    if (size > 0) {
        dst->data = malloc(size);
        memcpy(dst->data, data, size);
    }
    port->n_pending_events++;
}

int main(void)
{
    /* --- Whitebox round-trip: encode a control sequence from staged
     * events, then decode it back, and confirm every field survives. --- */
    struct tpw_filter_port scratch_out = { .media_type = TPW_STREAM_TYPE_EVENT, .direction = TPW_FILTER_PORT_OUTPUT };

    uint8_t midi_bytes[3] = { 0x90, 0x40, 0x7f };
    stage_pending(&scratch_out, TPW_EVENT_MIDI, NULL, midi_bytes, sizeof(midi_bytes), 10);

    const char osc_bytes[] = "/synth/freq";
    stage_pending(&scratch_out, TPW_EVENT_OSC, NULL, osc_bytes, sizeof(osc_bytes), 20);

    float vol = 0.75f;
    stage_pending(&scratch_out, TPW_EVENT_PROPERTY, "volume", &vol, sizeof(vol), 30);

    uint8_t encode_buf[1024];
    size_t encoded = tpw_filter_event_finish_output(&scratch_out, encode_buf, sizeof(encode_buf));
    TPW_ASSERT(encoded > 0);
    TPW_ASSERT_EQ(scratch_out.n_pending_events, (size_t)0);
    free(scratch_out.pending_events);

    struct tpw_filter_port scratch_in = { .media_type = TPW_STREAM_TYPE_EVENT, .direction = TPW_FILTER_PORT_INPUT };
    tpw_filter_event_decode(&scratch_in, encode_buf, encoded);
    TPW_ASSERT_EQ(scratch_in.n_incoming_events, (size_t)3);

    TPW_ASSERT_EQ(scratch_in.incoming_events[0].offset, (uint32_t)10);
    TPW_ASSERT_EQ(scratch_in.incoming_events[0].kind, TPW_EVENT_MIDI);
    TPW_ASSERT(scratch_in.incoming_events[0].key == NULL);
    TPW_ASSERT_EQ(scratch_in.incoming_events[0].size, sizeof(midi_bytes));
    TPW_ASSERT(memcmp(scratch_in.incoming_events[0].data, midi_bytes, sizeof(midi_bytes)) == 0);

    TPW_ASSERT_EQ(scratch_in.incoming_events[1].offset, (uint32_t)20);
    TPW_ASSERT_EQ(scratch_in.incoming_events[1].kind, TPW_EVENT_OSC);
    TPW_ASSERT_EQ(scratch_in.incoming_events[1].size, sizeof(osc_bytes));
    TPW_ASSERT(memcmp(scratch_in.incoming_events[1].data, osc_bytes, sizeof(osc_bytes)) == 0);

    TPW_ASSERT_EQ(scratch_in.incoming_events[2].offset, (uint32_t)30);
    TPW_ASSERT_EQ(scratch_in.incoming_events[2].kind, TPW_EVENT_PROPERTY);
    TPW_ASSERT(strcmp(scratch_in.incoming_events[2].key, "volume") == 0);
    TPW_ASSERT_EQ(scratch_in.incoming_events[2].size, sizeof(vol));
    float decoded_vol;
    memcpy(&decoded_vol, scratch_in.incoming_events[2].data, sizeof(decoded_vol));
    TPW_ASSERT_EQ(decoded_vol, vol);
    free(scratch_in.incoming_events);

    /* --- Zero-event decode is not an error. --- */
    struct tpw_filter_port scratch_empty = { .media_type = TPW_STREAM_TYPE_EVENT, .direction = TPW_FILTER_PORT_INPUT };
    tpw_filter_event_decode(&scratch_empty, NULL, 0);
    TPW_ASSERT_EQ(scratch_empty.n_incoming_events, (size_t)0);

    /* --- Public API: add ports, reject bad pushes, real cycle round
     * trip through the input-staging path. --- */
    tpw_filter_h filter = tpw_filter_create("tpw-test-event", process_cb, NULL);
    TPW_ASSERT(filter != NULL);

    tpw_filter_port_h ev_in = tpw_filter_add_event_port(filter, TPW_FILTER_PORT_INPUT);
    TPW_ASSERT(ev_in != NULL);
    tpw_filter_port_h ev_out = tpw_filter_add_event_port(filter, TPW_FILTER_PORT_OUTPUT);
    TPW_ASSERT(ev_out != NULL);
    tpw_filter_port_h audio_in = tpw_filter_add_audio_port(
        filter, TPW_FILTER_PORT_INPUT, &(tpw_audio_config){ .sample_rate = 48000, .channels = 2 });
    TPW_ASSERT(audio_in != NULL);

    /* get_event()/event_count() reject a non-event port, an output
     * event port (get_event() is input-only), and an out-of-range
     * index — none of this needs a real cycle to have run yet. */
    tpw_event scratch_ev;
    TPW_ASSERT_EQ(tpw_filter_port_get_event(audio_in, 0, &scratch_ev), TPW_ERR_INVALID_ARG);
    TPW_ASSERT_EQ(tpw_filter_port_get_event(ev_out, 0, &scratch_ev), TPW_ERR_INVALID_ARG);
    TPW_ASSERT_EQ(tpw_filter_port_get_event(ev_in, 0, &scratch_ev), TPW_ERR_INVALID_ARG);
    TPW_ASSERT_EQ(tpw_filter_port_get_event_count(audio_in), (size_t)0);
    TPW_ASSERT_EQ(tpw_filter_port_get_event_count(ev_out), (size_t)0);

    uint8_t junk[4] = { 0 };
    TPW_ASSERT_EQ(tpw_filter_push_port_data(filter, ev_in, junk, sizeof(junk), -1), TPW_ERR_INVALID_ARG);

    tpw_event bad_key_on_midi = { .offset = 0, .kind = TPW_EVENT_MIDI, .key = "volume", .data = NULL, .size = 0 };
    TPW_ASSERT_EQ(tpw_filter_port_push_event(ev_in, &bad_key_on_midi), TPW_ERR_INVALID_ARG);

    int one = 1;
    tpw_event unrecognized_key = { .offset = 0, .kind = TPW_EVENT_PROPERTY, .key = "not-a-real-property",
                                    .data = &one, .size = sizeof(one) };
    TPW_ASSERT_EQ(tpw_filter_port_push_event(ev_in, &unrecognized_key), TPW_ERR_INVALID_ARG);

    tpw_event unknown_kind = { .offset = 0, .kind = TPW_EVENT_UNKNOWN, .key = NULL, .data = NULL, .size = 0 };
    TPW_ASSERT_EQ(tpw_filter_port_push_event(ev_in, &unknown_kind), TPW_ERR_INVALID_ARG);

    TPW_ASSERT_EQ(tpw_filter_start(filter), TPW_OK);
    sleep(1);
    TPW_ASSERT_EQ(g_last_event_count, (size_t)0);

    uint8_t midi2[3] = { 0x80, 0x3c, 0x00 };
    tpw_event push_ev = { .offset = 5, .kind = TPW_EVENT_MIDI, .key = NULL, .data = midi2, .size = sizeof(midi2) };
    TPW_ASSERT_EQ(tpw_filter_port_push_event(ev_in, &push_ev), TPW_OK);
    sleep(1);

    TPW_ASSERT_EQ(g_last_event_count, (size_t)1);
    TPW_ASSERT_EQ(g_last_offset, (uint32_t)5);
    TPW_ASSERT_EQ(g_last_kind, TPW_EVENT_MIDI);
    TPW_ASSERT_EQ(g_last_size, sizeof(midi2));
    TPW_ASSERT(memcmp(g_last_data, midi2, sizeof(midi2)) == 0);

    TPW_ASSERT(g_cycles > 0);

    /* Can't reach the success path without a real link (see process_cb);
     * this only guards that the call stays within its documented contract. */
    TPW_ASSERT(g_output_push_calls > 0);
    TPW_ASSERT_EQ(g_output_push_unexpected, 0);

    tpw_filter_stop(filter, false);
    tpw_filter_destroy(filter);

    /* Adding an event port after the filter has started is rejected. */
    tpw_filter_h started = tpw_filter_create("tpw-test-event-started", process_cb, NULL);
    TPW_ASSERT(started != NULL);
    TPW_ASSERT(tpw_filter_add_audio_port(started, TPW_FILTER_PORT_INPUT,
                                          &(tpw_audio_config){ .sample_rate = 48000, .channels = 2 }) != NULL);
    TPW_ASSERT_EQ(tpw_filter_start(started), TPW_OK);
    TPW_ASSERT(tpw_filter_add_event_port(started, TPW_FILTER_PORT_OUTPUT) == NULL);
    tpw_filter_stop(started, false);
    tpw_filter_destroy(started);

    return 0;
}
