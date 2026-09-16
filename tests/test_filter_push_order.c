/* SPDX-License-Identifier: MIT */

/* Proves what test_filter_sensor_latency_hw only assumes: a multi-value
 * buffer pushed via tpw_filter_push_port_data() arrives at the callback
 * with its contents in exactly the order it was written, undisturbed by
 * the copy into and back out of port->pushed_data.
 *
 * A real sensor's own readings cannot show this — two adjacent samples of
 * a slow, coarsely-quantized sensor are often identical, so a reordering
 * bug would be invisible in the values themselves. Pushing a synthetic,
 * strictly increasing counter instead makes any reordering, duplication,
 * or corruption immediately visible: the values must come back in the
 * same ascending sequence they were written in, every time.
 *
 * Driven directly through tpw_filter_on_process() (whitebox, as
 * test_filter_callback_push.c already does), so this needs no real
 * PipeWire graph, no timing, and no hardware. */

#include <string.h>

#include "tpw_filter_internal.h"
#include "tpw_test.h"

static tpw_filter_port_h g_port;
static const float* g_seen;
static size_t g_seen_count;

static void on_process(tpw_filter_h filter, tpw_filter_port_buffer* buffers, size_t n, void* user_data)
{
    (void)filter;
    (void)user_data;
    g_seen = NULL;
    g_seen_count = 0;
    for (size_t i = 0; i < n; i++) {
        if (buffers[i].port != g_port || !buffers[i].data)
            continue;
        g_seen = (const float*)buffers[i].data;
        g_seen_count = buffers[i].size / sizeof(float);
    }
}

/* A single block arrives byte-for-byte, in the order it was written. */
static void test_single_block_order(void)
{
    tpw_filter_h handle = tpw_filter_create("tpw-test-push-order", on_process, NULL);
    TPW_ASSERT(handle != NULL);
    g_port = tpw_filter_add_signal_port(handle, TPW_FILTER_PORT_INPUT);
    TPW_ASSERT(g_port != NULL);

    float block[5] = { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f };
    TPW_ASSERT_EQ(tpw_filter_push_port_data(handle, g_port, block, sizeof(block), 0), TPW_OK);

    tpw_filter_on_process((struct tpw_filter*)handle, NULL);

    TPW_ASSERT_EQ(g_seen_count, (size_t)5);
    TPW_ASSERT(memcmp(g_seen, block, sizeof(block)) == 0);
    for (size_t i = 0; i < 5; i++)
        TPW_ASSERT_EQ(g_seen[i], (float)(i + 1));

    tpw_filter_destroy(handle);
}

/* Consecutive blocks continue the sequence: no reordering or mixing across
 * cycles, matching the batch-per-cycle pattern the sensor tests use. */
static void test_consecutive_blocks_continue_in_order(void)
{
    tpw_filter_h handle = tpw_filter_create("tpw-test-push-order-seq", on_process, NULL);
    TPW_ASSERT(handle != NULL);
    g_port = tpw_filter_add_signal_port(handle, TPW_FILTER_PORT_INPUT);
    TPW_ASSERT(g_port != NULL);

    float first[5] = { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f };
    TPW_ASSERT_EQ(tpw_filter_push_port_data(handle, g_port, first, sizeof(first), 0), TPW_OK);
    tpw_filter_on_process((struct tpw_filter*)handle, NULL);
    TPW_ASSERT_EQ(g_seen_count, (size_t)5);
    for (size_t i = 0; i < 5; i++)
        TPW_ASSERT_EQ(g_seen[i], (float)(i + 1));

    float second[5] = { 6.0f, 7.0f, 8.0f, 9.0f, 10.0f };
    TPW_ASSERT_EQ(tpw_filter_push_port_data(handle, g_port, second, sizeof(second), 0), TPW_OK);
    tpw_filter_on_process((struct tpw_filter*)handle, NULL);
    TPW_ASSERT_EQ(g_seen_count, (size_t)5);
    for (size_t i = 0; i < 5; i++)
        TPW_ASSERT_EQ(g_seen[i], (float)(i + 6));

    tpw_filter_destroy(handle);
}

/* A push that lands before the previous one is ever read replaces it
 * wholly: the callback must see exactly the newer block, never a mix of
 * old and new bytes. */
static void test_overwrite_replaces_whole_block(void)
{
    tpw_filter_h handle = tpw_filter_create("tpw-test-push-order-overwrite", on_process, NULL);
    TPW_ASSERT(handle != NULL);
    g_port = tpw_filter_add_signal_port(handle, TPW_FILTER_PORT_INPUT);
    TPW_ASSERT(g_port != NULL);

    float stale[5] = { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f };
    float fresh[2] = { 100.0f, 200.0f };
    TPW_ASSERT_EQ(tpw_filter_push_port_data(handle, g_port, stale, sizeof(stale), 0), TPW_OK);
    TPW_ASSERT_EQ(tpw_filter_push_port_data(handle, g_port, fresh, sizeof(fresh), 0), TPW_OK);

    tpw_filter_on_process((struct tpw_filter*)handle, NULL);

    /* Exactly the newer, smaller block — no leftover stale[] elements
     * bleeding in past the end of the shrunk buffer. */
    TPW_ASSERT_EQ(g_seen_count, (size_t)2);
    TPW_ASSERT_EQ(g_seen[0], 100.0f);
    TPW_ASSERT_EQ(g_seen[1], 200.0f);

    tpw_filter_destroy(handle);
}

/* The staging buffer grows and shrinks as pushes of different sizes reuse
 * it; a larger push after a smaller one must not surface bytes left over
 * from an even earlier, larger push. */
static void test_grow_shrink_grow_reuses_buffer_cleanly(void)
{
    tpw_filter_h handle = tpw_filter_create("tpw-test-push-order-reuse", on_process, NULL);
    TPW_ASSERT(handle != NULL);
    g_port = tpw_filter_add_signal_port(handle, TPW_FILTER_PORT_INPUT);
    TPW_ASSERT(g_port != NULL);

    float big1[10] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    TPW_ASSERT_EQ(tpw_filter_push_port_data(handle, g_port, big1, sizeof(big1), 0), TPW_OK);
    tpw_filter_on_process((struct tpw_filter*)handle, NULL);
    TPW_ASSERT_EQ(g_seen_count, (size_t)10);

    float small[2] = { 101.0f, 102.0f };
    TPW_ASSERT_EQ(tpw_filter_push_port_data(handle, g_port, small, sizeof(small), 0), TPW_OK);
    tpw_filter_on_process((struct tpw_filter*)handle, NULL);
    TPW_ASSERT_EQ(g_seen_count, (size_t)2);
    TPW_ASSERT_EQ(g_seen[0], 101.0f);
    TPW_ASSERT_EQ(g_seen[1], 102.0f);

    float big2[8] = { 201, 202, 203, 204, 205, 206, 207, 208 };
    TPW_ASSERT_EQ(tpw_filter_push_port_data(handle, g_port, big2, sizeof(big2), 0), TPW_OK);
    tpw_filter_on_process((struct tpw_filter*)handle, NULL);
    TPW_ASSERT_EQ(g_seen_count, (size_t)8);
    TPW_ASSERT(memcmp(g_seen, big2, sizeof(big2)) == 0);

    tpw_filter_destroy(handle);
}

int main(void)
{
    test_single_block_order();
    test_consecutive_blocks_continue_in_order();
    test_overwrite_replaces_whole_block();
    test_grow_shrink_grow_reuses_buffer_cleanly();
    return 0;
}
