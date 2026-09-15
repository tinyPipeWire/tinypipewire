/* SPDX-License-Identifier: MIT */

#include "tpw/tpw_filter.h"
#include "tpw_filter_internal.h" /* whitebox: the ns->ratio conversion */
#include "tpw_test.h"

static void noop_process_cb(tpw_filter_h filter, tpw_filter_port_buffer* buffers, size_t n_buffers,
                             void* user_data)
{
    (void)filter;
    (void)buffers;
    (void)n_buffers;
    (void)user_data;
}

/* The hint is a duration expressed as "num/48000"; whole milliseconds land
 * exactly (num == ms * 48), sub-tick values floor and clamp to >= 1. */
static void test_period_conversion(void)
{
    TPW_ASSERT_EQ(tpw_filter_period_hint_num(10000000), 480u); /* 10 ms */
    TPW_ASSERT_EQ(tpw_filter_period_hint_num(5000000), 240u);  /* 5 ms  */
    TPW_ASSERT_EQ(tpw_filter_period_hint_num(1000000), 48u);   /* 1 ms  */
    TPW_ASSERT_EQ(tpw_filter_period_hint_num(1000), 1u);       /* 1 us -> floored, clamped to 1 */
}

int main(void)
{
    test_period_conversion();

    tpw_filter_h filter = tpw_filter_create("tpw-test-period-hint", noop_process_cb, NULL);
    TPW_ASSERT(filter != NULL);

    tpw_filter_port_h in = tpw_filter_add_signal_port(filter, TPW_FILTER_PORT_INPUT);
    TPW_ASSERT(in != NULL);

    /* Accepted before start; 0 clears it. */
    TPW_ASSERT_EQ(tpw_filter_set_period_hint(filter, 10000000), TPW_STREAM_OK);
    TPW_ASSERT_EQ(tpw_filter_set_period_hint(filter, 0), TPW_STREAM_OK);
    TPW_ASSERT_EQ(tpw_filter_set_period_hint(filter, 10000000), TPW_STREAM_OK);

    TPW_ASSERT_EQ(tpw_filter_start(filter), TPW_STREAM_OK);

    /* Rejected after start (it is a connect-time node property). */
    TPW_ASSERT_EQ(tpw_filter_set_period_hint(filter, 5000000), TPW_STREAM_ERR_INVALID_ARG);

    tpw_filter_stop(filter, false);
    tpw_filter_destroy(filter);
    return 0;
}
