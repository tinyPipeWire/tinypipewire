/* SPDX-License-Identifier: MIT */

#include <unistd.h>

#include "tpw/tpw_filter.h"
#include "tpw_test.h"

/* Two signal input ports: one with hold enabled, one without. The process
 * callback fires continuously once started; counters are cumulative and
 * monotonic so they can be read from the main thread without resetting. */
static tpw_filter_port_h g_hold_port;
static tpw_filter_port_h g_nohold_port;

static int g_hold_fresh;       /* new-data deliveries on the hold port */
static int g_hold_held;        /* held re-presentations on the hold port */
static int g_hold_nodata;      /* cycles the hold port had no buffer */
static uint64_t g_hold_fresh_seq;   /* seq seen on the latest fresh delivery */
static uint64_t g_hold_held_seq;    /* seq seen on the latest held delivery */
static size_t g_hold_held_size;
static int64_t g_hold_held_pts;

static int g_nohold_fresh;
static int g_nohold_held;      /* MUST stay 0: hold is opt-in */

static void process_cb(tpw_filter_h filter, tpw_filter_port_buffer* buffers, size_t n_buffers, void* user_data)
{
    (void)filter;
    (void)user_data;
    for (size_t i = 0; i < n_buffers; i++) {
        tpw_filter_port_buffer* b = &buffers[i];
        bool is_hold = (b->port == g_hold_port);

        if (!b->data) {
            if (is_hold)
                g_hold_nodata++;
            continue;
        }
        if (b->fresh) {
            if (is_hold) {
                g_hold_fresh++;
                g_hold_fresh_seq = b->seq;
            } else {
                g_nohold_fresh++;
            }
        } else {
            if (is_hold) {
                g_hold_held++;
                g_hold_held_seq = b->seq;
                g_hold_held_size = b->size;
                g_hold_held_pts = b->pts;
            } else {
                g_nohold_held++;
            }
        }
    }
}

int main(void)
{
    tpw_filter_h filter = tpw_filter_create("tpw-test-hold", process_cb, NULL);
    TPW_ASSERT(filter != NULL);

    g_hold_port = tpw_filter_add_signal_port(filter, TPW_FILTER_PORT_INPUT);
    g_nohold_port = tpw_filter_add_signal_port(filter, TPW_FILTER_PORT_INPUT);
    TPW_ASSERT(g_hold_port != NULL);
    TPW_ASSERT(g_nohold_port != NULL);

    /* Hold is opt-in and only valid before start. */
    TPW_ASSERT_EQ(tpw_filter_port_set_hold(g_hold_port, true), TPW_STREAM_OK);

    TPW_ASSERT_EQ(tpw_filter_start(filter), TPW_STREAM_OK);

    /* Setting hold after start is rejected. */
    TPW_ASSERT_EQ(tpw_filter_port_set_hold(g_hold_port, true), TPW_STREAM_ERR_INVALID_ARG);

    /* Phase A: before any buffer, a hold port reports no buffer (not a
     * stale/invalid one) — same as a non-hold port. */
    usleep(400000);
    TPW_ASSERT_EQ(g_hold_fresh, 0);
    TPW_ASSERT_EQ(g_hold_held, 0);
    TPW_ASSERT(g_hold_nodata > 0);

    /* Phase B: one push to each port. Exactly one fresh delivery per push;
     * the hold port then re-presents that buffer on later cycles. */
    float v1 = 0.25f;
    TPW_ASSERT_EQ(tpw_filter_push_port_data(filter, g_hold_port, &v1, sizeof(v1), 1000), TPW_STREAM_OK);
    TPW_ASSERT_EQ(tpw_filter_push_port_data(filter, g_nohold_port, &v1, sizeof(v1), 1000), TPW_STREAM_OK);
    usleep(500000);

    TPW_ASSERT_EQ(g_hold_fresh, 1);
    TPW_ASSERT_EQ(g_hold_fresh_seq, 1);
    TPW_ASSERT(g_hold_held > 0);          /* re-presented on empty cycles */
    TPW_ASSERT_EQ(g_hold_held_seq, 1);    /* seq unchanged while held */
    TPW_ASSERT_EQ(g_hold_held_size, sizeof(float));
    TPW_ASSERT_EQ(g_hold_held_pts, (int64_t)1000);

    TPW_ASSERT_EQ(g_nohold_fresh, 1);
    TPW_ASSERT_EQ(g_nohold_held, 0);      /* non-hold never re-presents */

    /* Phase C: a new push advances seq and updates the held payload. */
    float v2 = 0.75f;
    TPW_ASSERT_EQ(tpw_filter_push_port_data(filter, g_hold_port, &v2, sizeof(v2), 2000), TPW_STREAM_OK);
    usleep(500000);

    TPW_ASSERT_EQ(g_hold_fresh, 2);
    TPW_ASSERT_EQ(g_hold_fresh_seq, 2);
    TPW_ASSERT_EQ(g_hold_held_seq, 2);
    TPW_ASSERT_EQ(g_hold_held_pts, (int64_t)2000);
    TPW_ASSERT_EQ(g_nohold_held, 0);

    tpw_filter_stop(filter, false);
    tpw_filter_destroy(filter);
    return 0;
}
