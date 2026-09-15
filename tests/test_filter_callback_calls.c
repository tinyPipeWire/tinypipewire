/* SPDX-License-Identifier: MIT */

/* Software filters linked by name stand in for a device, so a real lost source
 * fires the error callback and no hardware or session manager is needed. */

#include <stdatomic.h>
#include <time.h>
#include <unistd.h>

#include "tpw_filter_internal.h"
#include "tpw_test.h"

enum { CALL_START, CALL_STOP, CALL_LINK, CALL_UNLINK, CALL_FORMATS, N_CALLS };

struct sink {
    tpw_filter_h filter;
    tpw_filter_port_h in;
    tpw_filter_port_h spare;
    bool calls_in_process;
    bool calls_in_error;
    atomic_int cycles;
    atomic_int errors;
    atomic_int calls_done;
    int results[N_CALLS];
    double link_ms;
};

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* Polls `counter` for up to three seconds and reports whether it reached `target`. */
static bool wait_for(const atomic_int* counter, int target)
{
    for (int i = 0; i < 300; i++) {
        if (atomic_load(counter) >= target)
            return true;
        usleep(10000);
    }
    return false;
}

static void source_cb(tpw_filter_h filter, tpw_filter_port_buffer* buffers, size_t n, void* user_data)
{
    (void)filter;
    (void)user_data;
    for (size_t i = 0; i < n; i++)
        buffers[i].size = 0;
}

static tpw_filter_h make_source(const char* name)
{
    tpw_filter_h filter = tpw_filter_create(name, source_cb, NULL);
    TPW_ASSERT(filter != NULL);
    TPW_ASSERT(tpw_filter_add_signal_port(filter, TPW_FILTER_PORT_OUTPUT) != NULL);
    TPW_ASSERT_EQ(tpw_filter_start(filter), TPW_STREAM_OK);
    return filter;
}

/* Every call here either takes the loop lock or waits on the loop, so each must be refused. */
static void call_everything(struct sink* s)
{
    size_t found = 0;
    s->results[CALL_START] = tpw_filter_start(s->filter);
    s->results[CALL_STOP] = tpw_filter_stop(s->filter, false);
    s->results[CALL_UNLINK] = tpw_filter_port_unlink(s->in);
    s->results[CALL_FORMATS] = tpw_filter_get_target_video_formats(s->filter, "tpw-test-cbc-src", NULL, 0, &found);
    double t0 = now_ms();
    s->results[CALL_LINK] = tpw_filter_port_link(s->spare, "tpw-test-cbc-src");
    s->link_ms = now_ms() - t0;
    tpw_filter_destroy(s->filter); /* The call is refused, so the filter stays usable. */
}

static void sink_cb(tpw_filter_h filter, tpw_filter_port_buffer* buffers, size_t n, void* user_data)
{
    (void)filter;
    (void)buffers;
    (void)n;
    struct sink* s = user_data;
    if (atomic_fetch_add(&s->cycles, 1) == 10 && s->calls_in_process) {
        call_everything(s);
        atomic_store(&s->calls_done, 1);
    }
}

static void sink_error_cb(tpw_filter_h filter, tpw_filter_port_h port, int error_code, void* user_data)
{
    (void)filter;
    (void)port;
    (void)error_code;
    struct sink* s = user_data;
    if (atomic_fetch_add(&s->errors, 1) == 0 && s->calls_in_error) {
        size_t found = 0;
        s->results[CALL_FORMATS] = tpw_filter_get_target_video_formats(s->filter, "tpw-test-cbc-src", NULL, 0, &found);
        double t0 = now_ms();
        s->results[CALL_LINK] = tpw_filter_port_link(s->spare, "tpw-test-cbc-src");
        s->link_ms = now_ms() - t0;
        tpw_filter_destroy(s->filter); /* The call is refused, so the filter stays usable. */
        s->results[CALL_STOP] = tpw_filter_stop(s->filter, false); /* A stop is allowed on the loop thread. */
        atomic_store(&s->calls_done, 1);
    }
}

static void make_sink(struct sink* s, const char* name)
{
    s->filter = tpw_filter_create(name, sink_cb, s);
    TPW_ASSERT(s->filter != NULL);
    s->in = tpw_filter_add_signal_port(s->filter, TPW_FILTER_PORT_INPUT);
    s->spare = tpw_filter_add_signal_port(s->filter, TPW_FILTER_PORT_INPUT);
    TPW_ASSERT(s->in != NULL && s->spare != NULL);
    TPW_ASSERT_EQ(tpw_filter_set_error_cb(s->filter, sink_error_cb), TPW_STREAM_OK);
    TPW_ASSERT_EQ(tpw_filter_start(s->filter), TPW_STREAM_OK);
}

/* Links by the name given to create(), retrying while the new node reaches the registry. */
static int link_by_name(tpw_filter_port_h port, const char* name)
{
    int res = TPW_STREAM_ERR_INVALID_ARG;
    for (int i = 0; i < 20 && res != TPW_STREAM_OK; i++) {
        res = tpw_filter_port_link(port, name);
        if (res != TPW_STREAM_OK)
            usleep(100000);
    }
    return res;
}

/* A source that disappears is reported once, though both its link and its format go. */
static void test_lost_source_reported_once(void)
{
    tpw_filter_h src = make_source("tpw-test-cbc-lost");
    struct sink s = { 0 };
    make_sink(&s, "tpw-test-cbc-sink-lost");
    TPW_ASSERT_EQ(link_by_name(s.in, "tpw-test-cbc-lost"), TPW_STREAM_OK);
    usleep(200000);

    tpw_filter_destroy(src);
    TPW_ASSERT(wait_for(&s.errors, 1));
    usleep(300000);
    TPW_ASSERT_EQ(atomic_load(&s.errors), 1);
    tpw_filter_destroy(s.filter);
}

/* The application's own unlink, stop or destroy loses no source, so none reports one. */
static void test_own_teardown_reports_nothing(void)
{
    tpw_filter_h src = make_source("tpw-test-cbc-src");

    for (int how = 0; how < 3; how++) {
        struct sink s = { 0 };
        make_sink(&s, "tpw-test-cbc-sink-own");
        TPW_ASSERT_EQ(link_by_name(s.in, "tpw-test-cbc-src"), TPW_STREAM_OK);
        usleep(200000);

        if (how == 0)
            TPW_ASSERT_EQ(tpw_filter_port_unlink(s.in), TPW_STREAM_OK);
        else if (how == 1)
            TPW_ASSERT_EQ(tpw_filter_stop(s.filter, false), TPW_STREAM_OK);
        usleep(300000);
        if (how == 2)
            tpw_filter_destroy(s.filter);
        TPW_ASSERT_EQ(atomic_load(&s.errors), 0);
        if (how != 2)
            tpw_filter_destroy(s.filter);
    }
    tpw_filter_destroy(src);
}

/* The process callback runs on the data thread, where every call taking the loop lock is refused. */
static void test_calls_refused_in_process_callback(void)
{
    tpw_filter_h src = make_source("tpw-test-cbc-src");
    struct sink s = { .calls_in_process = true };
    make_sink(&s, "tpw-test-cbc-sink-process");
    TPW_ASSERT_EQ(link_by_name(s.in, "tpw-test-cbc-src"), TPW_STREAM_OK);

    TPW_ASSERT(wait_for(&s.calls_done, 1));
    for (int i = 0; i < N_CALLS; i++)
        TPW_ASSERT_EQ(s.results[i], TPW_STREAM_ERR_IN_CALLBACK);
    TPW_ASSERT(s.link_ms < 1000.0);

    int cycles = atomic_load(&s.cycles);
    TPW_ASSERT(wait_for(&s.cycles, cycles + 5)); /* The refused destroy left it running. */
    tpw_filter_destroy(s.filter);
    tpw_filter_destroy(src);
}

/* The error callback runs on the loop thread, where calls that would wait on it are refused. */
static void test_calls_refused_in_error_callback(void)
{
    tpw_filter_h src = make_source("tpw-test-cbc-gone");
    tpw_filter_h other = make_source("tpw-test-cbc-src");
    struct sink s = { .calls_in_error = true };
    make_sink(&s, "tpw-test-cbc-sink-error");
    TPW_ASSERT_EQ(link_by_name(s.in, "tpw-test-cbc-gone"), TPW_STREAM_OK);
    usleep(200000);

    tpw_filter_destroy(src);
    TPW_ASSERT(wait_for(&s.calls_done, 1));
    TPW_ASSERT_EQ(s.results[CALL_FORMATS], TPW_STREAM_ERR_IN_CALLBACK);
    TPW_ASSERT_EQ(s.results[CALL_LINK], TPW_STREAM_ERR_IN_CALLBACK);
    TPW_ASSERT(s.link_ms < 1000.0); /* It is refused at once rather than after the link timeout. */
    TPW_ASSERT_EQ(s.results[CALL_STOP], TPW_STREAM_OK);

    TPW_ASSERT_EQ(tpw_filter_start(s.filter), TPW_STREAM_OK); /* The refused destroy left it usable. */
    tpw_filter_destroy(s.filter);
    tpw_filter_destroy(other);
}

int main(void)
{
    test_lost_source_reported_once();
    test_own_teardown_reports_nothing();
    test_calls_refused_in_process_callback();
    test_calls_refused_in_error_callback();
    printf("test_filter_callback_calls: all cases passed\n");
    return 0;
}
