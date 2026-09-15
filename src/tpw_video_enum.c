/* SPDX-License-Identifier: MIT */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <pipewire/node.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/iter.h>

#include "tpw_log_internal.h"
#include "tpw_pw_core_internal.h"
#include "tpw_spa_format_internal.h"

/* Collects one node's EnumFormat results while the caller blocks on the
 * core round-trip that follows the request. */
struct tpw_video_enum_ctx {
    tpw_video_format_info* out;
    size_t out_len;
    size_t found; /* the true count, which may run past out_len */
};

void tpw_video_insert_framerate(tpw_video_format_info* info, int fps)
{
    const size_t cap = sizeof(info->fps) / sizeof(info->fps[0]);
    size_t pos = 0;

    for (size_t i = 0; i < info->n_fps; i++) {
        if (info->fps[i] == fps)
            return; /* a range whose ends meet, or a repeated default */
    }

    while (pos < info->n_fps && info->fps[pos] > fps)
        pos++;
    if (pos == cap)
        return; /* the list is full and every rate in it is faster */

    size_t last = info->n_fps < cap ? info->n_fps : cap - 1;
    for (size_t i = last; i > pos; i--)
        info->fps[i] = info->fps[i - 1];
    info->fps[pos] = fps;
    if (info->n_fps < cap)
        info->n_fps++;
}

/* Names one EnumFormat object's pixel format; NULL for an encoded subtype
 * with no name here, or a raw format this library cannot express. */
static const char* tpw_video_parse_pixel_format(const struct spa_pod* param)
{
    uint32_t media_type, media_subtype;
    if (spa_format_parse(param, &media_type, &media_subtype) < 0)
        return NULL;
    if (media_type != SPA_MEDIA_TYPE_video)
        return NULL;
    if (media_subtype == SPA_MEDIA_SUBTYPE_mjpg)
        return "MJPG";
    if (media_subtype == SPA_MEDIA_SUBTYPE_h264)
        return "H264";
    if (media_subtype != SPA_MEDIA_SUBTYPE_raw)
        return NULL;

    const struct spa_pod_prop* prop = spa_pod_find_prop(param, NULL, SPA_FORMAT_VIDEO_format);
    if (!prop)
        return NULL;

    /* A V4L2 source emits one object per pixel format, so this is a plain
     * Id; take the default value if some other source made it a choice. */
    uint32_t n_vals, choice;
    struct spa_pod* values = spa_pod_get_values(&prop->value, &n_vals, &choice);
    if (!values || values->type != SPA_TYPE_Id || n_vals == 0)
        return NULL;

    const uint32_t* ids = SPA_POD_BODY(values);
    return tpw_spa_pixel_format_name_or_null((enum spa_video_format)ids[0]);
}

/* A discrete size is a bare rectangle and leaves the maxima equal; a Range
 * or Step choice carries [default, min, max, ...], so the ends are 1 and 2. */
static bool tpw_video_parse_size(const struct spa_pod* param, tpw_video_format_info* info)
{
    const struct spa_pod_prop* prop = spa_pod_find_prop(param, NULL, SPA_FORMAT_VIDEO_size);
    if (!prop)
        return false;

    uint32_t n_vals, choice;
    struct spa_pod* values = spa_pod_get_values(&prop->value, &n_vals, &choice);
    if (!values || values->type != SPA_TYPE_Rectangle || n_vals == 0)
        return false;

    const struct spa_rectangle* rects = SPA_POD_BODY(values);
    bool ranged = (choice == SPA_CHOICE_Range || choice == SPA_CHOICE_Step) && n_vals >= 3;
    const struct spa_rectangle* low = ranged ? &rects[1] : &rects[0];
    const struct spa_rectangle* high = ranged ? &rects[2] : &rects[0];

    if (low->width == 0 || low->height == 0)
        return false;

    info->width = (int)low->width;
    info->height = (int)low->height;
    info->width_max = (int)high->width;
    info->height_max = (int)high->height;
    return true;
}

/* An Enum repeats the default at entry 0, so rates start at 1; a Range has
 * its ends at 1 and 2. Sub-1fps rounds to 0, which means "auto", so it goes. */
static void tpw_video_parse_framerates(const struct spa_pod* param, tpw_video_format_info* info)
{
    const struct spa_pod_prop* prop = spa_pod_find_prop(param, NULL, SPA_FORMAT_VIDEO_framerate);
    if (!prop)
        return;

    uint32_t n_vals, choice;
    struct spa_pod* values = spa_pod_get_values(&prop->value, &n_vals, &choice);
    if (!values || values->type != SPA_TYPE_Fraction || n_vals == 0)
        return;

    const struct spa_fraction* fracs = SPA_POD_BODY(values);
    uint32_t first = 0, last = 0;

    switch (choice) {
    case SPA_CHOICE_None:
        first = last = 0;
        break;
    case SPA_CHOICE_Enum:
        if (n_vals < 2)
            return;
        first = 1;
        last = n_vals - 1;
        break;
    case SPA_CHOICE_Range:
    case SPA_CHOICE_Step:
        /* Only the ends are nameable; everything between them is legal too. */
        if (n_vals < 3)
            return;
        first = 1;
        last = 2;
        break;
    default:
        return;
    }

    for (uint32_t i = first; i <= last; i++) {
        if (fracs[i].denom == 0)
            continue;
        int fps = (int)(fracs[i].num / fracs[i].denom);
        if (fps > 0)
            tpw_video_insert_framerate(info, fps);
    }
}

static void tpw_video_enum_on_param(void* data, int seq, uint32_t id, uint32_t index, uint32_t next,
                                     const struct spa_pod* param)
{
    struct tpw_video_enum_ctx* ctx = data;
    (void)seq;
    (void)index;
    (void)next;

    if (id != SPA_PARAM_EnumFormat || !param)
        return;

    const char* pixel_format = tpw_video_parse_pixel_format(param);
    if (!pixel_format)
        return;

    /* Parse into a scratch entry first: a malformed object must not leave a
     * half-written slot behind, and counting it would overstate the total. */
    tpw_video_format_info info;
    memset(&info, 0, sizeof(info));
    if (!tpw_video_parse_size(param, &info))
        return;
    tpw_video_parse_framerates(param, &info);
    snprintf(info.pixel_format, sizeof(info.pixel_format), "%s", pixel_format);

    if (ctx->out && ctx->found < ctx->out_len)
        ctx->out[ctx->found] = info;
    ctx->found++;
}

static const struct pw_node_events tpw_video_enum_node_events = {
    PW_VERSION_NODE_EVENTS,
    .param = tpw_video_enum_on_param,
};

int tpw_pw_enum_video_formats(struct tpw_pw_core_conn* conn, struct tpw_pw_registry* reg,
                               uint32_t node_id, tpw_video_format_info* out, size_t out_len,
                               size_t* found)
{
    if (!found)
        return TPW_STREAM_ERR_INVALID_ARG;

    *found = 0;
    if (!conn || !conn->core || !reg || !reg->registry || node_id == 0)
        return TPW_STREAM_ERR_INVALID_ARG;

    struct tpw_video_enum_ctx ctx = { .out = out, .out_len = out ? out_len : 0, .found = 0 };

    pw_thread_loop_lock(conn->loop);

    struct pw_node* node =
        pw_registry_bind(reg->registry, node_id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0);
    if (!node) {
        pw_thread_loop_unlock(conn->loop);
        tpw_log_error("failed to bind node %u to read its formats", node_id);
        return TPW_STREAM_ERR_CONNECT_FAILED;
    }

    struct spa_hook listener;
    spa_zero(listener);
    pw_node_add_listener(node, &listener, &tpw_video_enum_node_events, &ctx);
    pw_node_enum_params(node, 0, SPA_PARAM_EnumFormat, 0, UINT32_MAX, NULL);

    /* Every param event the request produces is delivered before the sync
     * completes, so one round-trip is the whole enumeration. */
    int res = tpw_pw_core_sync_locked(conn);

    spa_hook_remove(&listener);
    pw_proxy_destroy((struct pw_proxy*)node);
    pw_thread_loop_unlock(conn->loop);

    if (res < 0) {
        tpw_log_error("reading the formats of node %u failed (result=%d)", node_id, res);
        return tpw_pw_error_from_sync(res);
    }

    *found = ctx.found;
    return TPW_STREAM_OK;
}
