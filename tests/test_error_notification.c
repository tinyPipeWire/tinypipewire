/* SPDX-License-Identifier: MIT */

/* Whitebox test: exercises the state_changed -> error_cb wiring directly
 * by simulating a PipeWire stream-state transition, rather than
 * physically removing a device. A real end-to-end device-loss check is
 * documented separately in the project's quickstart guide. */

#include "tpw_stream_internal.h"
#include "tpw_test.h"

static int g_error_calls = 0;
static int g_last_error_code = 0;

static void noop_data_cb(tpw_stream_h stream, const tpw_stream_buffer* buf, void* user_data)
{
    (void)stream;
    (void)buf;
    (void)user_data;
}

static void on_error(tpw_stream_h stream, int error_code, void* user_data)
{
    (void)stream;
    (void)user_data;
    g_error_calls++;
    g_last_error_code = error_code;
}

static void check_source_loss(tpw_stream_h handle)
{
    struct tpw_stream* stream = (struct tpw_stream*)handle;

    stream->state = TPW_STREAM_STATE_RUNNING;
    g_error_calls = 0;

    tpw_stream_on_state_changed(stream, PW_STREAM_STATE_STREAMING, PW_STREAM_STATE_ERROR,
                                 "simulated source loss");

    TPW_ASSERT_EQ(g_error_calls, 1);
    TPW_ASSERT_EQ(g_last_error_code, TPW_ERR_SOURCE_UNAVAILABLE);
    TPW_ASSERT_EQ(stream->state, TPW_STREAM_STATE_STOPPED);

    /* No further error is raised once already stopped. */
    tpw_stream_on_state_changed(stream, PW_STREAM_STATE_UNCONNECTED, PW_STREAM_STATE_ERROR,
                                 "simulated source loss again");
    TPW_ASSERT_EQ(g_error_calls, 1);
}

int main(void)
{
    tpw_stream_h audio = tpw_stream_create(TPW_STREAM_TYPE_AUDIO, noop_data_cb, NULL);
    TPW_ASSERT(audio != NULL);
    TPW_ASSERT_EQ(tpw_stream_set_error_cb(audio, on_error), TPW_OK);
    check_source_loss(audio);
    tpw_stream_destroy(audio);

    tpw_stream_h video = tpw_stream_create(TPW_STREAM_TYPE_VIDEO, noop_data_cb, NULL);
    TPW_ASSERT(video != NULL);
    TPW_ASSERT_EQ(tpw_stream_set_error_cb(video, on_error), TPW_OK);
    check_source_loss(video);
    tpw_stream_destroy(video);

    return 0;
}
