/* SPDX-License-Identifier: MIT */

#include <unistd.h>

#include "tpw/tpw_stream.h"
#include "tpw_test.h"

static void ignore_data_cb(tpw_stream_h stream, const tpw_stream_buffer* buf, void* user_data)
{
    (void)stream;
    (void)buf;
    (void)user_data;
}

int main(void)
{
    /* NULL handle is rejected regardless of target. */
    TPW_ASSERT_EQ(tpw_stream_set_target(NULL, "some-node"), TPW_ERR_INVALID_ARG);

    /* A target naming a node that doesn't exist must not break the
     * connect/start/stop lifecycle: PW_KEY_TARGET_OBJECT only steers
     * PipeWire's auto-link policy, it isn't validated at connect time. */
    tpw_stream_h s = tpw_stream_create(TPW_STREAM_TYPE_AUDIO, ignore_data_cb, NULL);
    TPW_ASSERT(s != NULL);
    TPW_ASSERT_EQ(tpw_stream_set_target(s, "tpw-test-nonexistent-node"), TPW_OK);
    TPW_ASSERT_EQ(tpw_stream_set_audio_config(s, &(tpw_audio_config){ .sample_rate = 48000, .channels = 2 }), TPW_OK);
    TPW_ASSERT_EQ(tpw_stream_start(s), TPW_OK);
    sleep(1);
    TPW_ASSERT_EQ(tpw_stream_stop(s, false), TPW_OK);

    /* Clearing back to NULL (falls back to auto-connect) is also accepted. */
    TPW_ASSERT_EQ(tpw_stream_set_target(s, NULL), TPW_OK);
    tpw_stream_destroy(s);

    return 0;
}
