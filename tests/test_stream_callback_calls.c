/* SPDX-License-Identifier: MIT */

/* A stream reaches its callbacks only once linked, so the data callback is simulated
 * by its marker and the loop thread is entered through an invoke instead. */

#include <stdatomic.h>
#include <time.h>
#include <unistd.h>

#include "tpw_stream_internal.h"
#include "tpw_test.h"

static const tpw_audio_config g_cfg = { .sample_rate = 48000, .channels = 2 };

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void on_data(tpw_stream_h stream, const tpw_stream_buffer* buf, void* user_data)
{
    (void)stream;
    (void)buf;
    (void)user_data;
}

static tpw_stream_h make_running_stream(void)
{
    tpw_stream_h s = tpw_stream_create(TPW_STREAM_TYPE_AUDIO, on_data, NULL);
    TPW_ASSERT(s != NULL);
    TPW_ASSERT_EQ(tpw_stream_set_autoconnect(s, false), TPW_STREAM_OK);
    TPW_ASSERT_EQ(tpw_stream_set_audio_config(s, &g_cfg), TPW_STREAM_OK);
    TPW_ASSERT_EQ(tpw_stream_start(s), TPW_STREAM_OK);
    return s;
}

/* Inside the data callback every call taking the loop lock is refused at once. */
static void test_calls_refused_in_data_callback(void)
{
    tpw_stream_h s = make_running_stream();
    size_t found = 99;

    tpw_stream_processing = (struct tpw_stream*)s;
    TPW_ASSERT_EQ(tpw_stream_start(s), TPW_STREAM_ERR_INVALID_ARG);
    TPW_ASSERT_EQ(tpw_stream_stop(s, false), TPW_STREAM_ERR_INVALID_ARG);
    TPW_ASSERT_EQ(tpw_stream_set_audio_config(s, &g_cfg), TPW_STREAM_ERR_INVALID_ARG);
    TPW_ASSERT_EQ(tpw_stream_get_target_list(s, NULL, 0, &found), TPW_STREAM_ERR_INVALID_ARG);
    double t0 = now_ms();
    TPW_ASSERT_EQ(tpw_stream_link(s, "tpw-test-no-such-node"), TPW_STREAM_ERR_INVALID_ARG);
    TPW_ASSERT(now_ms() - t0 < 1000.0); /* It is refused instead of waiting for the stream's ports. */
    tpw_stream_destroy(s);
    tpw_stream_processing = NULL;

    TPW_ASSERT_EQ(tpw_stream_set_role(s, "Music"), TPW_STREAM_OK); /* The refused destroy left it alive. */
    TPW_ASSERT_EQ(tpw_stream_stop(s, false), TPW_STREAM_OK);
    tpw_stream_destroy(s);
}

struct loop_calls {
    struct tpw_stream* stream;
    int in_loop_thread;
    int link;
    double link_ms;
    int targets;
    int config;
    int stop_drain;
    int stop;
    atomic_int done;
};

static int call_on_loop(struct spa_loop* loop, bool async, uint32_t seq, const void* data, size_t size,
                        void* user_data)
{
    (void)loop;
    (void)async;
    (void)seq;
    (void)data;
    (void)size;
    struct loop_calls* c = user_data;
    tpw_stream_h s = (tpw_stream_h)c->stream;
    size_t found = 0;

    c->in_loop_thread = pw_thread_loop_in_thread(c->stream->conn.loop);
    tpw_stream_destroy(s); /* It is refused, so the calls below still have a stream. */
    double t0 = now_ms();
    c->link = tpw_stream_link(s, "tpw-test-no-such-node");
    c->link_ms = now_ms() - t0;
    c->targets = tpw_stream_get_target_list(s, NULL, 0, &found);
    c->config = tpw_stream_set_audio_config(s, &g_cfg);
    c->stop_drain = tpw_stream_stop(s, true);
    c->stop = tpw_stream_stop(s, false);
    atomic_store(&c->done, 1);
    return 0;
}

/* On the loop thread, where the error callback runs, calls that would wait on it are refused. */
static void test_calls_refused_on_loop_thread(void)
{
    tpw_stream_h s = make_running_stream();
    struct loop_calls c = { .stream = (struct tpw_stream*)s, .in_loop_thread = -1 };

    /* A blocking invoke from outside breaks the thread loop's lock count, so this one queues and polls. */
    pw_thread_loop_lock(c.stream->conn.loop);
    pw_loop_invoke(pw_thread_loop_get_loop(c.stream->conn.loop), call_on_loop, 0, NULL, 0, false, &c);
    pw_thread_loop_unlock(c.stream->conn.loop);
    for (int i = 0; i < 300 && !atomic_load(&c.done); i++)
        usleep(10000);

    TPW_ASSERT(atomic_load(&c.done));
    TPW_ASSERT_EQ(c.in_loop_thread, 1);
    TPW_ASSERT_EQ(c.link, TPW_STREAM_ERR_INVALID_ARG);
    TPW_ASSERT(c.link_ms < 1000.0);
    TPW_ASSERT_EQ(c.targets, TPW_STREAM_ERR_INVALID_ARG);
    TPW_ASSERT_EQ(c.config, TPW_STREAM_ERR_INVALID_ARG);
    TPW_ASSERT_EQ(c.stop_drain, TPW_STREAM_ERR_INVALID_ARG);
    TPW_ASSERT_EQ(c.stop, TPW_STREAM_OK); /* A stop that does not drain waits on nothing. */
    tpw_stream_destroy(s);
}

int main(void)
{
    test_calls_refused_in_data_callback();
    test_calls_refused_on_loop_thread();
    printf("test_stream_callback_calls: all cases passed\n");
    return 0;
}
