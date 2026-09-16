/* SPDX-License-Identifier: MIT */

#include "tpw/tpw_stream.h"
#include "tpw_test.h"

static void noop_data_cb(tpw_stream_h stream, const tpw_stream_buffer* buf, void* user_data)
{
    (void)stream;
    (void)buf;
    (void)user_data;
}

int main(void)
{
    tpw_stream_h stream = tpw_stream_create(TPW_STREAM_TYPE_AUDIO, noop_data_cb, NULL);
    TPW_ASSERT(stream != NULL);

    /* A NULL config is rejected. */
    TPW_ASSERT_EQ(tpw_stream_set_audio_config(stream, NULL), TPW_ERR_INVALID_ARG);

    /* Invalid formats are rejected without starting the stream. */
    TPW_ASSERT_EQ(tpw_stream_set_audio_config(stream, &(tpw_audio_config){ .sample_rate = 0, .channels = 2 }), TPW_ERR_INVALID_FORMAT);
    TPW_ASSERT_EQ(tpw_stream_set_audio_config(stream, &(tpw_audio_config){ .sample_rate = 48000, .channels = 0 }), TPW_ERR_INVALID_FORMAT);
    TPW_ASSERT_EQ(tpw_stream_set_audio_config(stream, &(tpw_audio_config){ .sample_rate = -1, .channels = 2 }), TPW_ERR_INVALID_FORMAT);
    TPW_ASSERT_EQ(tpw_stream_set_audio_config(stream, &(tpw_audio_config){ .sample_rate = 48000, .channels = 2, .format = "NOT_A_FORMAT" }), TPW_ERR_INVALID_FORMAT);
    TPW_ASSERT_EQ(tpw_stream_start(stream), TPW_ERR_NOT_CONFIGURED);

    /* A valid config is accepted, whether it leaves format NULL (defaults
     * to "S16") or names a supported sample format explicitly. */
    TPW_ASSERT_EQ(tpw_stream_set_audio_config(stream, &(tpw_audio_config){ .sample_rate = 48000, .channels = 2 }), TPW_OK);
    TPW_ASSERT_EQ(tpw_stream_set_audio_config(stream, &(tpw_audio_config){ .sample_rate = 48000, .channels = 2, .format = "F32" }), TPW_OK);
    TPW_ASSERT_EQ(tpw_stream_set_audio_config(stream, &(tpw_audio_config){ .sample_rate = 48000, .channels = 2, .format = "U8" }), TPW_OK);
    TPW_ASSERT_EQ(tpw_stream_set_audio_config(stream, &(tpw_audio_config){ .sample_rate = 48000, .channels = 2, .format = "S24_32" }), TPW_OK);

    tpw_stream_destroy(stream);
    return 0;
}
