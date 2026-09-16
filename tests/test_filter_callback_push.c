/* SPDX-License-Identifier: MIT */

/* Pushing from inside the processing callback must not take the filter's
 * thread-loop lock — doing so deadlocks against PipeWire's buffer setup.
 * The lock is skipped based on tpw_filter_processing, so these tests pin
 * that marker's behaviour directly instead of relying on a hang, which
 * only reproduces under parallel load and would pass on an idle machine. */

#include <unistd.h>

#include "tpw_filter_internal.h"
#include "tpw_test.h"

static const tpw_audio_config g_cfg = { .sample_rate = 48000, .channels = 2 };

/* --- marker is set during the callback and restored after ------------- */

static struct tpw_filter* g_expect;
static const struct tpw_filter* g_seen;
static int g_marker_calls;

static void marker_cb(tpw_filter_h filter, tpw_filter_port_buffer* buffers, size_t n, void* user_data)
{
    (void)buffers;
    (void)n;
    (void)user_data;
    (void)filter;
    g_marker_calls++;
    g_seen = tpw_filter_processing;
}

static void test_marker_set_and_restored(void)
{
    tpw_filter_h handle = tpw_filter_create("tpw-test-cb-marker", marker_cb, NULL);
    TPW_ASSERT(handle != NULL);
    TPW_ASSERT(tpw_filter_add_audio_port(handle, TPW_FILTER_PORT_INPUT, &g_cfg) != NULL);

    struct tpw_filter* filter = (struct tpw_filter*)handle;
    g_expect = filter;

    /* Nothing is running on this thread yet. */
    TPW_ASSERT(tpw_filter_processing == NULL);

    /* Drive the process callback directly so the whole set/restore happens
     * on this thread, where it can be observed. */
    tpw_filter_on_process(filter, NULL);

    TPW_ASSERT_EQ(g_marker_calls, 1);
    TPW_ASSERT(g_seen == filter);           /* set while the callback ran */
    TPW_ASSERT(tpw_filter_processing == NULL); /* and restored afterwards */

    tpw_filter_destroy(handle);
}

/* --- a nested filter restores the outer one, not NULL ----------------- */

static struct tpw_filter* g_inner;
static const struct tpw_filter* g_seen_inner;
static const struct tpw_filter* g_seen_after_inner;

static void inner_cb(tpw_filter_h filter, tpw_filter_port_buffer* buffers, size_t n, void* user_data)
{
    (void)filter;
    (void)buffers;
    (void)n;
    (void)user_data;
    g_seen_inner = tpw_filter_processing;
}

static void outer_cb(tpw_filter_h filter, tpw_filter_port_buffer* buffers, size_t n, void* user_data)
{
    (void)filter;
    (void)buffers;
    (void)n;
    (void)user_data;
    tpw_filter_on_process(g_inner, NULL);
    g_seen_after_inner = tpw_filter_processing;
}

static void test_nested_filter_restores_outer(void)
{
    tpw_filter_h outer_h = tpw_filter_create("tpw-test-cb-outer", outer_cb, NULL);
    tpw_filter_h inner_h = tpw_filter_create("tpw-test-cb-inner", inner_cb, NULL);
    TPW_ASSERT(outer_h != NULL && inner_h != NULL);
    TPW_ASSERT(tpw_filter_add_audio_port(outer_h, TPW_FILTER_PORT_INPUT, &g_cfg) != NULL);
    TPW_ASSERT(tpw_filter_add_audio_port(inner_h, TPW_FILTER_PORT_INPUT, &g_cfg) != NULL);

    struct tpw_filter* outer = (struct tpw_filter*)outer_h;
    g_inner = (struct tpw_filter*)inner_h;

    tpw_filter_on_process(outer, NULL);

    /* Each callback sees its own filter, and the inner one hands the marker
     * back to the outer rather than clearing it — otherwise a push in the
     * rest of the outer callback would wrongly take the lock. */
    TPW_ASSERT(g_seen_inner == g_inner);
    TPW_ASSERT(g_seen_after_inner == outer);
    TPW_ASSERT(tpw_filter_processing == NULL);

    tpw_filter_destroy(outer_h);
    tpw_filter_destroy(inner_h);
}

/* --- push_port_data from inside the callback ------------------------- */

static tpw_filter_port_h g_push_port;
static int g_push_result = 1;      /* 1 = never attempted */
static int g_push_cycles;
static float g_delivered = -1.0f;

static void push_cb(tpw_filter_h filter, tpw_filter_port_buffer* buffers, size_t n, void* user_data)
{
    (void)user_data;
    g_push_cycles++;

    for (size_t i = 0; i < n; i++) {
        if (buffers[i].port == g_push_port && buffers[i].data && buffers[i].size >= sizeof(float))
            g_delivered = *(const float*)buffers[i].data;
    }

    /* Push once, from inside the callback: this is the path that used to
     * take the loop lock on the data thread and deadlock. */
    if (g_push_result == 1) {
        float v = 0.5f;
        g_push_result = tpw_filter_push_port_data(filter, g_push_port, &v, sizeof(v), 42);
    }
}

static void test_push_port_data_from_callback(void)
{
    tpw_filter_h handle = tpw_filter_create("tpw-test-cb-push", push_cb, NULL);
    TPW_ASSERT(handle != NULL);
    g_push_port = tpw_filter_add_signal_port(handle, TPW_FILTER_PORT_INPUT);
    TPW_ASSERT(g_push_port != NULL);

    struct tpw_filter* filter = (struct tpw_filter*)handle;

    /* Two cycles: the first pushes, the second must see what it staged. */
    tpw_filter_on_process(filter, NULL);
    TPW_ASSERT_EQ(g_push_result, TPW_OK);
    tpw_filter_on_process(filter, NULL);

    TPW_ASSERT_EQ(g_push_cycles, 2);
    TPW_ASSERT_EQ(g_delivered, 0.5f);

    tpw_filter_destroy(handle);
}

/* --- pushing to a different filter still locks normally --------------- */

static struct tpw_filter* g_other;
static tpw_filter_port_h g_other_port;
static int g_cross_result = 1;

static void cross_cb(tpw_filter_h filter, tpw_filter_port_buffer* buffers, size_t n, void* user_data)
{
    (void)filter;
    (void)buffers;
    (void)n;
    (void)user_data;
    if (g_cross_result == 1) {
        float v = 1.5f;
        /* A different filter's loop is not the one running us, so this one
         * does take its lock — it must still succeed and not hang. */
        g_cross_result = tpw_filter_push_port_data((tpw_filter_h)g_other, g_other_port, &v, sizeof(v), -1);
    }
}

static void test_push_to_other_filter_from_callback(void)
{
    tpw_filter_h driver = tpw_filter_create("tpw-test-cb-cross-a", cross_cb, NULL);
    tpw_filter_h other = tpw_filter_create("tpw-test-cb-cross-b", marker_cb, NULL);
    TPW_ASSERT(driver != NULL && other != NULL);
    TPW_ASSERT(tpw_filter_add_signal_port(driver, TPW_FILTER_PORT_INPUT) != NULL);
    g_other_port = tpw_filter_add_signal_port(other, TPW_FILTER_PORT_INPUT);
    TPW_ASSERT(g_other_port != NULL);
    g_other = (struct tpw_filter*)other;

    tpw_filter_on_process((struct tpw_filter*)driver, NULL);
    TPW_ASSERT_EQ(g_cross_result, TPW_OK);

    tpw_filter_destroy(driver);
    tpw_filter_destroy(other);
}

/* --- app-thread pushes still work while the filter runs --------------- */

static void quiet_cb(tpw_filter_h filter, tpw_filter_port_buffer* buffers, size_t n, void* user_data)
{
    (void)filter;
    (void)buffers;
    (void)n;
    (void)user_data;
}

static void test_app_thread_push_still_works(void)
{
    tpw_filter_h handle = tpw_filter_create("tpw-test-cb-appthread", quiet_cb, NULL);
    TPW_ASSERT(handle != NULL);
    tpw_filter_port_h in = tpw_filter_add_signal_port(handle, TPW_FILTER_PORT_INPUT);
    TPW_ASSERT(in != NULL);
    TPW_ASSERT_EQ(tpw_filter_start(handle), TPW_OK);

    /* The marker is per-thread, so an app-thread push takes the lock as
     * before even while the filter's own thread is processing. */
    float v = 2.5f;
    TPW_ASSERT(tpw_filter_processing == NULL);
    TPW_ASSERT_EQ(tpw_filter_push_port_data(handle, in, &v, sizeof(v), -1), TPW_OK);

    tpw_filter_stop(handle, false);
    tpw_filter_destroy(handle);
}

int main(void)
{
    test_marker_set_and_restored();
    test_nested_filter_restores_outer();
    test_push_port_data_from_callback();
    test_push_to_other_filter_from_callback();
    test_app_thread_push_still_works();
    return 0;
}
