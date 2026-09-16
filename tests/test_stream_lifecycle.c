/* SPDX-License-Identifier: MIT */

#include <unistd.h>

#include "tpw/tpw_stream.h"
#include "tpw_test.h"

static int g_data_calls = 0;

static void count_data_cb(tpw_stream_h stream, const tpw_stream_buffer* buf, void* user_data)
{
    (void)stream;
    (void)buf;
    (void)user_data;
    g_data_calls++;
}

int main(void)
{
    /* start() before a format is set must be rejected. */
    tpw_stream_h s1 = tpw_stream_create(TPW_DATA_AUDIO, count_data_cb, NULL);
    TPW_ASSERT(s1 != NULL);
    TPW_ASSERT_EQ(tpw_stream_start(s1), TPW_ERR_NOT_CONFIGURED);
    tpw_stream_destroy(s1);

    /* Full lifecycle: create -> set format -> start -> stop -> restart -> destroy. */
    tpw_stream_h s2 = tpw_stream_create(TPW_DATA_AUDIO, count_data_cb, NULL);
    TPW_ASSERT(s2 != NULL);
    TPW_ASSERT_EQ(tpw_stream_set_audio_config(s2, &(tpw_audio_config){ .sample_rate = 48000, .channels = 2 }), TPW_OK);

    TPW_ASSERT_EQ(tpw_stream_start(s2), TPW_OK);
    sleep(1);
    TPW_ASSERT_EQ(tpw_stream_stop(s2, false), TPW_OK);
    int calls_after_stop = g_data_calls;
    sleep(1);
    TPW_ASSERT_EQ(g_data_calls, calls_after_stop); /* no delivery once stopped */

    TPW_ASSERT_EQ(tpw_stream_start(s2), TPW_OK); /* restart after stop */
    sleep(1);
    TPW_ASSERT_EQ(tpw_stream_stop(s2, false), TPW_OK);
    tpw_stream_destroy(s2);

    /* destroy() while running must stop delivery and release resources safely. */
    tpw_stream_h s3 = tpw_stream_create(TPW_DATA_AUDIO, count_data_cb, NULL);
    TPW_ASSERT(s3 != NULL);
    TPW_ASSERT_EQ(tpw_stream_set_audio_config(s3, &(tpw_audio_config){ .sample_rate = 48000, .channels = 2 }), TPW_OK);
    TPW_ASSERT_EQ(tpw_stream_start(s3), TPW_OK);
    tpw_stream_destroy(s3);

    /* One audio stream and one video stream running concurrently; stopping
     * or destroying one must not affect the other. */
    tpw_stream_h audio = tpw_stream_create(TPW_DATA_AUDIO, count_data_cb, NULL);
    tpw_stream_h video = tpw_stream_create(TPW_DATA_VIDEO, count_data_cb, NULL);
    TPW_ASSERT(audio != NULL);
    TPW_ASSERT(video != NULL);
    TPW_ASSERT_EQ(tpw_stream_set_audio_config(audio, &(tpw_audio_config){ .sample_rate = 48000, .channels = 2 }), TPW_OK);
    TPW_ASSERT_EQ(tpw_stream_set_video_config(video, &(tpw_video_config){ .width = 640, .height = 480, .pixel_format = "RGB" }), TPW_OK);
    TPW_ASSERT_EQ(tpw_stream_start(audio), TPW_OK);
    TPW_ASSERT_EQ(tpw_stream_start(video), TPW_OK);
    sleep(1);

    tpw_stream_destroy(audio);
    sleep(1);
    TPW_ASSERT_EQ(tpw_stream_stop(video, false), TPW_OK); /* video unaffected by audio's destroy */
    tpw_stream_destroy(video);

    return 0;
}
