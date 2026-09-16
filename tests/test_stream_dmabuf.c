/* SPDX-License-Identifier: MIT */

#include <spa/buffer/buffer.h>

#include "tpw/tpw_stream.h"
#include "tpw_stream_internal.h" /* whitebox: feed a synthetic DMABUF buffer, inspect use_dmabuf */
#include "tpw_test.h"

static void noop_data_cb(tpw_stream_h stream, const tpw_stream_buffer* buf, void* user_data)
{
    (void)stream;
    (void)buf;
    (void)user_data;
}

static void noop_playback_cb(tpw_stream_h stream, tpw_stream_playback_buffer* buf, void* user_data)
{
    (void)stream;
    (void)buf;
    (void)user_data;
}

/* Feeds a synthetic buffer straight to the accessor and checks it extracts
 * the DMABUF plane's fd/offset/stride/size, skipping a non-DMABUF block. */
static void test_plane_extraction(tpw_stream_h handle)
{
    struct tpw_stream* stream = (struct tpw_stream*)handle;

    struct spa_chunk chunk0 = { .offset = 0, .size = 100, .stride = 640 };
    struct spa_data datas[2] = {
        { .type = SPA_DATA_DmaBuf, .fd = 42, .mapoffset = 16, .maxsize = 200, .chunk = &chunk0 },
        { .type = SPA_DATA_MemPtr, .fd = -1 },
    };
    struct spa_buffer sb = { .n_datas = 2, .datas = datas };
    stream->current_dmabuf_buf = &sb;

    tpw_dmabuf_plane planes[4];
    TPW_ASSERT_EQ(tpw_stream_get_dmabuf_planes(handle, planes, 4), (size_t)1);
    TPW_ASSERT_EQ(planes[0].fd, 42);
    TPW_ASSERT_EQ(planes[0].offset, 16u);
    TPW_ASSERT_EQ(planes[0].stride, 640u);
    TPW_ASSERT_EQ(planes[0].size, 100u);
    TPW_ASSERT(stream->dmabuf_retrieved);

    stream->current_dmabuf_buf = NULL;
}

/* NV12: two planes as two distinct file descriptors. */
static void test_nv12_two_fds(tpw_stream_h handle)
{
    struct tpw_stream* stream = (struct tpw_stream*)handle;

    struct spa_chunk chunk_y = { .size = 640 * 480, .stride = 640 };
    struct spa_chunk chunk_uv = { .size = 640 * 480 / 2, .stride = 640 };
    struct spa_data datas[2] = {
        { .type = SPA_DATA_DmaBuf, .fd = 10, .mapoffset = 0, .maxsize = 640 * 480, .chunk = &chunk_y },
        { .type = SPA_DATA_DmaBuf, .fd = 11, .mapoffset = 0, .maxsize = 640 * 480 / 2, .chunk = &chunk_uv },
    };
    struct spa_buffer sb = { .n_datas = 2, .datas = datas };
    stream->current_dmabuf_buf = &sb;

    tpw_dmabuf_plane planes[4];
    TPW_ASSERT_EQ(tpw_stream_get_dmabuf_planes(handle, planes, 4), (size_t)2);
    TPW_ASSERT_EQ(planes[0].fd, 10);
    TPW_ASSERT_EQ(planes[0].stride, 640u);
    TPW_ASSERT_EQ(planes[1].fd, 11);
    TPW_ASSERT_EQ(planes[1].stride, 640u);

    stream->current_dmabuf_buf = NULL;
}

/* I420: three planes sharing one file descriptor at different offsets. */
static void test_i420_shared_fd(tpw_stream_h handle)
{
    struct tpw_stream* stream = (struct tpw_stream*)handle;

    struct spa_chunk chunk_y = { .size = 640 * 480, .stride = 640 };
    struct spa_chunk chunk_u = { .size = 640 * 480 / 4, .stride = 320 };
    struct spa_chunk chunk_v = { .size = 640 * 480 / 4, .stride = 320 };
    struct spa_data datas[3] = {
        { .type = SPA_DATA_DmaBuf, .fd = 20, .mapoffset = 0, .maxsize = 640 * 480, .chunk = &chunk_y },
        { .type = SPA_DATA_DmaBuf,
          .fd = 20,
          .mapoffset = 640 * 480,
          .maxsize = 640 * 480 / 4,
          .chunk = &chunk_u },
        { .type = SPA_DATA_DmaBuf,
          .fd = 20,
          .mapoffset = 640 * 480 + 640 * 480 / 4,
          .maxsize = 640 * 480 / 4,
          .chunk = &chunk_v },
    };
    struct spa_buffer sb = { .n_datas = 3, .datas = datas };
    stream->current_dmabuf_buf = &sb;

    tpw_dmabuf_plane planes[4];
    TPW_ASSERT_EQ(tpw_stream_get_dmabuf_planes(handle, planes, 4), (size_t)3);
    TPW_ASSERT_EQ(planes[0].fd, 20);
    TPW_ASSERT_EQ(planes[0].offset, 0u);
    TPW_ASSERT_EQ(planes[1].fd, 20);
    TPW_ASSERT_EQ(planes[1].offset, (uint32_t)(640 * 480));
    TPW_ASSERT_EQ(planes[1].stride, 320u);
    TPW_ASSERT_EQ(planes[2].fd, 20);
    TPW_ASSERT_EQ(planes[2].offset, (uint32_t)(640 * 480 + 640 * 480 / 4));
    TPW_ASSERT_EQ(planes[2].stride, 320u);

    stream->current_dmabuf_buf = NULL;
}

int main(void)
{
    tpw_stream_h stream = tpw_stream_create(TPW_STREAM_TYPE_VIDEO, noop_data_cb, NULL);
    TPW_ASSERT(stream != NULL);

    tpw_video_config cfg = { .width = 640, .height = 480, .pixel_format = "RGB", .fps = 30 };

    /* opts == NULL is exactly the non-_ex call: no DMABUF is requested. */
    TPW_ASSERT_EQ(tpw_stream_set_video_config_ex(stream, &cfg, NULL), TPW_OK);
    TPW_ASSERT(!((struct tpw_stream*)stream)->use_dmabuf);

    /* A plain caller that has never heard of the _ex call or DMABUF stays
     * exactly as it was before this feature existed. */
    TPW_ASSERT_EQ(tpw_stream_set_video_config(stream, &cfg), TPW_OK);
    TPW_ASSERT(!((struct tpw_stream*)stream)->use_dmabuf);

    /* The accessor never fabricates a plane on a non-DMABUF stream, or for
     * a NULL handle. */
    tpw_dmabuf_plane planes[4];
    TPW_ASSERT_EQ(tpw_stream_get_dmabuf_planes(stream, planes, 4), 0);
    TPW_ASSERT_EQ(tpw_stream_get_dmabuf_planes(NULL, planes, 4), 0);

    /* DMABUF is accepted on a video capture stream. */
    tpw_stream_dmabuf_opts dmabuf_opts = { .memory = TPW_PORT_MEMORY_DMABUF };
    TPW_ASSERT_EQ(tpw_stream_set_video_config_ex(stream, &cfg, &dmabuf_opts), TPW_OK);
    TPW_ASSERT(((struct tpw_stream*)stream)->use_dmabuf);

    /* Outside a cycle (no current buffer) the accessor still returns 0. */
    TPW_ASSERT_EQ(tpw_stream_get_dmabuf_planes(stream, planes, 4), 0);

    /* With a real DMABUF frame present, the accessor extracts its plane. */
    test_plane_extraction(stream);

    /* Multi-plane pixel formats expose every plane, in both layouts a
     * source may use: distinct file descriptors, or one descriptor at
     * different offsets. */
    test_nv12_two_fds(stream);
    test_i420_shared_fd(stream);

    tpw_stream_destroy(stream);

    /* DMABUF is video-capture-only: rejected on an audio stream and on a
     * playback stream, the same guard tpw_stream_set_video_config() already
     * applies. */
    tpw_stream_h audio = tpw_stream_create(TPW_STREAM_TYPE_AUDIO, noop_data_cb, NULL);
    TPW_ASSERT(audio != NULL);
    TPW_ASSERT_EQ(tpw_stream_set_video_config_ex(audio, &cfg, &dmabuf_opts), TPW_ERR_INVALID_ARG);
    tpw_stream_destroy(audio);

    tpw_stream_h playback = tpw_stream_create_playback(noop_playback_cb, NULL);
    TPW_ASSERT(playback != NULL);
    TPW_ASSERT_EQ(tpw_stream_set_video_config_ex(playback, &cfg, &dmabuf_opts), TPW_ERR_INVALID_ARG);
    tpw_stream_destroy(playback);

    /* MJPEG and H.264 frames are never handed out as DMABUF. */
    tpw_stream_h mjpg = tpw_stream_create(TPW_STREAM_TYPE_VIDEO, noop_data_cb, NULL);
    TPW_ASSERT(mjpg != NULL);
    tpw_video_config mjpg_cfg = { .width = 640, .height = 480, .pixel_format = "MJPG", .fps = 30 };
    TPW_ASSERT_EQ(tpw_stream_set_video_config_ex(mjpg, &mjpg_cfg, &dmabuf_opts), TPW_ERR_INVALID_ARG);
    tpw_stream_destroy(mjpg);

    tpw_stream_h h264 = tpw_stream_create(TPW_STREAM_TYPE_VIDEO, noop_data_cb, NULL);
    TPW_ASSERT(h264 != NULL);
    tpw_video_config h264_cfg = { .width = 640, .height = 480, .pixel_format = "H264", .fps = 30 };
    TPW_ASSERT_EQ(tpw_stream_set_video_config_ex(h264, &h264_cfg, &dmabuf_opts), TPW_ERR_INVALID_ARG);
    tpw_stream_destroy(h264);

    return 0;
}
