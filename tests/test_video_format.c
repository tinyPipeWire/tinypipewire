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
    tpw_stream_h stream = tpw_stream_create(TPW_STREAM_TYPE_VIDEO, noop_data_cb, NULL);
    TPW_ASSERT(stream != NULL);

    /* A NULL config is rejected. */
    TPW_ASSERT_EQ(tpw_stream_set_video_config(stream, NULL), TPW_ERR_INVALID_ARG);

    /* Invalid dimensions, unrecognized pixel formats, and negative fps are rejected. */
    TPW_ASSERT_EQ(tpw_stream_set_video_config(stream, &(tpw_video_config){ .width = 0, .height = 480, .pixel_format = "RGB" }), TPW_ERR_INVALID_FORMAT);
    TPW_ASSERT_EQ(tpw_stream_set_video_config(stream, &(tpw_video_config){ .width = 640, .height = -1, .pixel_format = "RGB" }), TPW_ERR_INVALID_FORMAT);
    TPW_ASSERT_EQ(tpw_stream_set_video_config(stream, &(tpw_video_config){ .width = 640, .height = 480, .pixel_format = "NOT_A_FORMAT" }), TPW_ERR_INVALID_FORMAT);
    TPW_ASSERT_EQ(tpw_stream_set_video_config(stream, &(tpw_video_config){ .width = 640, .height = 480, .pixel_format = "RGB", .fps = -1 }), TPW_ERR_INVALID_FORMAT);
    TPW_ASSERT_EQ(tpw_stream_start(stream), TPW_ERR_NOT_CONFIGURED);

    /* A valid config (with an explicit frame rate) is accepted. */
    TPW_ASSERT_EQ(tpw_stream_set_video_config(stream, &(tpw_video_config){ .width = 640, .height = 480, .pixel_format = "RGB", .fps = 30 }), TPW_OK);

    tpw_stream_destroy(stream);

    /* Every supported pixel format is recognized. */
    static const char* supported_formats[] = { "RGB", "YUYV", "NV12", "NV21", "I420", "MJPG", "H264" };
    for (size_t i = 0; i < sizeof(supported_formats) / sizeof(supported_formats[0]); i++) {
        tpw_stream_h s = tpw_stream_create(TPW_STREAM_TYPE_VIDEO, noop_data_cb, NULL);
        TPW_ASSERT(s != NULL);
        TPW_ASSERT_EQ(tpw_stream_set_video_config(s, &(tpw_video_config){ .width = 640, .height = 480, .pixel_format = supported_formats[i], .fps = 30 }), TPW_OK);
        tpw_stream_destroy(s);
    }

    /* Only the FourCC spelling "MJPG" is recognized, not "MJPEG". */
    tpw_stream_h mjpeg_spelling = tpw_stream_create(TPW_STREAM_TYPE_VIDEO, noop_data_cb, NULL);
    TPW_ASSERT(mjpeg_spelling != NULL);
    TPW_ASSERT_EQ(tpw_stream_set_video_config(mjpeg_spelling, &(tpw_video_config){ .width = 640, .height = 480, .pixel_format = "MJPEG", .fps = 30 }),
                  TPW_ERR_INVALID_FORMAT);
    tpw_stream_destroy(mjpeg_spelling);

    return 0;
}
