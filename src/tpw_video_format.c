/* SPDX-License-Identifier: MIT */

#include "tpw_spa_format_internal.h"
#include "tpw_stream_internal.h"

int tpw_stream_set_video_config_ex(tpw_stream_h handle, const tpw_video_config* config,
                                    const tpw_stream_dmabuf_opts* opts)
{
    struct tpw_stream* stream = (struct tpw_stream*)handle;
    if (!stream || stream->type != TPW_DATA_VIDEO || !config || !config->pixel_format)
        return TPW_ERR_INVALID_ARG;
    if (tpw_stream_refuse_in_callback(stream, true, __func__))
        return TPW_ERR_IN_CALLBACK;
    /* Playback is audio-only, so a video format has nothing to connect to. */
    if (stream->direction != TPW_STREAM_DIRECTION_CAPTURE)
        return TPW_ERR_INVALID_ARG;
    if (config->width <= 0 || config->height <= 0 || config->fps < 0)
        return TPW_ERR_INVALID_FORMAT;

    bool use_dmabuf = opts && opts->memory == TPW_PORT_MEMORY_DMABUF;
    bool is_mjpg = tpw_spa_pixel_format_is_mjpg(config->pixel_format);
    bool is_h264 = tpw_spa_pixel_format_is_h264(config->pixel_format);
    bool is_encoded = is_mjpg || is_h264;
    /* Encoded frames (MJPEG, H.264) are never handed out as DMABUF; nothing
     * here would ever negotiate it, so refuse the request. */
    if (is_encoded && use_dmabuf)
        return TPW_ERR_INVALID_ARG;

    enum spa_video_format fmt = SPA_VIDEO_FORMAT_ENCODED;
    if (!is_encoded) {
        fmt = tpw_spa_lookup_pixel_format(config->pixel_format);
        if (fmt == SPA_VIDEO_FORMAT_UNKNOWN)
            return TPW_ERR_INVALID_FORMAT;
    }

    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod* params[3];
    if (is_mjpg)
        params[0] = tpw_spa_build_video_format_mjpg(&b, config);
    else if (is_h264)
        params[0] = tpw_spa_build_video_format_h264(&b, config);
    else
        params[0] = tpw_spa_build_video_format(&b, config, fmt);
    params[1] = tpw_spa_build_meta_header(&b);
    /* DMABUF's dataType is applied later in param_changed (add-time crashes
     * 1.0.5); restricting it here too would fight that deferred param. */
    uint32_t n_params = 2;
    if (!use_dmabuf)
        params[n_params++] = tpw_spa_build_cpu_buffers(&b);

    /* tpw_stream_internal_connect() owns stream->use_dmabuf: it must set
     * it only once the new pw_stream exists, not before. */
    int res = tpw_stream_internal_connect(stream, params, n_params, use_dmabuf);
    if (res < 0)
        return res;

    stream->format.video.width = config->width;
    stream->format.video.height = config->height;
    stream->format.video.format = fmt;
    stream->format_set = true;
    stream->state = TPW_STREAM_STATE_FORMAT_SET;
    return TPW_OK;
}

int tpw_stream_set_video_config(tpw_stream_h handle, const tpw_video_config* config)
{
    return tpw_stream_set_video_config_ex(handle, config, NULL);
}
