/* SPDX-License-Identifier: MIT */

/* Pushes race the cycle on PipeWire's data thread, and a deliberately slow
 * callback widens that window so a lost or torn push shows up reliably. */

#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

#include "tpw_filter_internal.h"
#include "tpw_test.h"

#define PUSHES 400
#define SLOW_CALLBACK_US 3000

/* Polls `counter` for up to five seconds and reports whether it got there. */
static bool wait_for(const atomic_uint* counter, unsigned target)
{
    for (int i = 0; i < 500; i++) {
        if (atomic_load(counter) >= target)
            return true;
        usleep(10000);
    }
    return false;
}

/* Every pushed event is delivered once, in push order. */

static atomic_uint g_events_seen;
static atomic_uint g_events_misordered;

static void event_cb(tpw_filter_h filter, tpw_filter_port_buffer* buffers, size_t n, void* user_data)
{
    (void)filter;
    (void)n;
    (void)user_data;

    tpw_filter_port_h in = buffers[0].port;
    size_t count = tpw_filter_port_get_event_count(in);
    for (size_t i = 0; i < count; i++) {
        tpw_event ev;
        uint32_t value = 0;
        if (tpw_filter_port_get_event(in, i, &ev) == TPW_OK && ev.size == sizeof(value))
            memcpy(&value, ev.data, sizeof(value));
        /* A lost event shifts every later value off its expected position. */
        if (value != atomic_load(&g_events_seen))
            atomic_fetch_add(&g_events_misordered, 1);
        atomic_fetch_add(&g_events_seen, 1);
    }
    usleep(SLOW_CALLBACK_US);
}

static void test_no_event_is_lost(void)
{
    tpw_filter_h filter = tpw_filter_create("tpw-test-push-race-events", event_cb, NULL);
    TPW_ASSERT(filter != NULL);
    tpw_filter_port_h in = tpw_filter_add_event_port(filter, TPW_FILTER_PORT_INPUT);
    TPW_ASSERT(in != NULL);
    TPW_ASSERT_EQ(tpw_filter_start(filter), TPW_OK);

    for (uint32_t i = 0; i < PUSHES; i++) {
        tpw_event ev = { .offset = 0, .kind = TPW_EVENT_MIDI, .key = NULL, .data = &i, .size = sizeof(i) };
        TPW_ASSERT_EQ(tpw_filter_port_push_event(in, &ev), TPW_OK);
        usleep(300 + (i % 7) * 300); /* The spacing lands pushes across the whole cycle. */
    }

    bool all_seen = wait_for(&g_events_seen, PUSHES);
    tpw_filter_stop(filter, false);
    tpw_filter_destroy(filter);

    unsigned seen = atomic_load(&g_events_seen);
    unsigned misordered = atomic_load(&g_events_misordered);
    TPW_ASSERT(all_seen);
    TPW_ASSERT_EQ(seen, (unsigned)PUSHES);
    TPW_ASSERT_EQ(misordered, 0u);
}

/* A delivered buffer does not change while the callback reads it. */

static atomic_uint g_data_fresh;
static atomic_uint g_data_torn;

/* Each push is filled with one byte value, and its size follows from it. */
static size_t size_for(uint8_t value)
{
    return 16 + (size_t)value * 8;
}

static void data_cb(tpw_filter_h filter, tpw_filter_port_buffer* buffers, size_t n, void* user_data)
{
    (void)filter;
    (void)n;
    (void)user_data;

    const tpw_filter_port_buffer* b = &buffers[0];
    if (!b->fresh || !b->data || b->size == 0) {
        usleep(SLOW_CALLBACK_US);
        return;
    }

    const uint8_t* bytes = b->data;
    uint8_t value = bytes[0];
    usleep(SLOW_CALLBACK_US); /* A push landing now must not touch these bytes. */

    bool torn = b->size != size_for(value);
    for (size_t i = 0; i < b->size && !torn; i++)
        torn = bytes[i] != value;
    if (torn)
        atomic_fetch_add(&g_data_torn, 1);
    atomic_fetch_add(&g_data_fresh, 1);
}

static void test_delivered_data_is_stable(void)
{
    tpw_filter_h filter = tpw_filter_create("tpw-test-push-race-data", data_cb, NULL);
    TPW_ASSERT(filter != NULL);
    tpw_filter_port_h in = tpw_filter_add_signal_port(filter, TPW_FILTER_PORT_INPUT);
    TPW_ASSERT(in != NULL);
    TPW_ASSERT_EQ(tpw_filter_start(filter), TPW_OK);

    uint8_t buf[16 + 255 * 8];
    for (unsigned i = 0; i < PUSHES; i++) {
        /* Sizes vary, so the push side regrows its buffer while capacity catches up. */
        uint8_t value = (uint8_t)(1 + (i * 37) % 255);
        memset(buf, value, size_for(value));
        TPW_ASSERT_EQ(tpw_filter_push_port_data(filter, in, buf, size_for(value), i), TPW_OK);
        usleep(300 + (i % 7) * 300);
    }

    bool delivered = wait_for(&g_data_fresh, 1);
    tpw_filter_stop(filter, false);
    tpw_filter_destroy(filter);

    unsigned torn = atomic_load(&g_data_torn);
    TPW_ASSERT(delivered);
    TPW_ASSERT_EQ(torn, 0u);
}

int main(void)
{
    test_no_event_is_lost();
    test_delivered_data_is_stable();
    printf("test_filter_push_race: all cases passed\n");
    return 0;
}
