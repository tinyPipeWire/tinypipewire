/* SPDX-License-Identifier: MIT */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pipewire/keys.h>
#include <spa/utils/dict.h>

#include "tpw_filter_internal.h"
#include "tpw_log_internal.h"
#include "tpw_spa_format_internal.h"

/* How long tpw_filter_stop(..., true) waits for a flush to actually drain
 * before giving up and stopping anyway. */
#define TPW_FILTER_DRAIN_TIMEOUT_NSEC (5 * SPA_NSEC_PER_SEC)

/* Reports a port that negotiated a format other than the one it asked for,
 * which happens when another consumer already configured the source. The
 * frame rate is not compared: fps == 0 asks the source to choose one. */
static void tpw_filter_check_negotiated_video(struct tpw_filter* filter, struct tpw_filter_port* port,
                                               const struct spa_pod* param)
{
    uint32_t media_type, media_subtype;
    if (port->media_type != TPW_STREAM_TYPE_VIDEO)
        return;
    if (spa_format_parse(param, &media_type, &media_subtype) < 0 || media_type != SPA_MEDIA_TYPE_video)
        return;

    enum spa_video_format got = SPA_VIDEO_FORMAT_ENCODED;
    struct spa_rectangle size = SPA_RECTANGLE(0, 0);
    if (media_subtype == SPA_MEDIA_SUBTYPE_raw) {
        struct spa_video_info_raw info;
        if (spa_format_video_raw_parse(param, &info) < 0)
            return;
        got = info.format;
        size = info.size;
    } else if (spa_pod_parse_object(param, SPA_TYPE_OBJECT_Format, NULL, SPA_FORMAT_VIDEO_size,
                                     SPA_POD_Rectangle(&size)) < 0) {
        return;
    }

    const struct tpw_filter_video_port_state* want = &port->config.video;
    if (got == want->format && (int)size.width == want->width && (int)size.height == want->height)
        return;

    tpw_log_warning("filter '%s': port negotiated %s %ux%u, not the %s %dx%d it requested; the source "
                    "is shared and was already configured",
                    filter->name ? filter->name : "tpw-filter", tpw_spa_pixel_format_name(got), size.width,
                    size.height, tpw_spa_pixel_format_name(want->format), want->width, want->height);
}

void tpw_filter_on_param_changed(void* data, void* port_data, uint32_t id, const struct spa_pod* param)
{
    struct tpw_filter* filter = data;
    struct tpw_filter_port* port = port_data;

    if (id != SPA_PARAM_Format || !port)
        return;

    if (param != NULL) {
        /* Format negotiated. A DMABUF port advertises its DmaBuf Buffers
         * param now (deferred from add time, which crashes 1.0.5); a
         * no-op for every other port. */
        port->loss_reported = false;
        tpw_filter_check_negotiated_video(filter, port, param);
        tpw_filter_dmabuf_update_params(port);
        return;
    }

    /* param == NULL: the port's negotiated format was cleared — its source
     * is gone, or a DMABUF port's source could not provide DMABUF. */
    /* An output port's format also clears when its consumer leaves, which loses no source. */
    if (port->direction != TPW_FILTER_PORT_INPUT || !tpw_filter_claim_source_loss(filter, port))
        return;
    if (port->use_dmabuf)
        tpw_filter_dmabuf_log_unavailable(port);
    else
        tpw_log_warning("filter '%s': a port's source became unavailable",
                        filter->name ? filter->name : "tpw-filter");

    if (filter->error_cb)
        filter->error_cb((tpw_filter_h)filter, (tpw_filter_port_h)port, TPW_STREAM_ERR_SOURCE_UNAVAILABLE,
                          filter->user_data);
}

bool tpw_filter_claim_source_loss(struct tpw_filter* filter, struct tpw_filter_port* port)
{
    if (filter->state != TPW_FILTER_STATE_RUNNING || port->loss_reported)
        return false;
    port->loss_reported = true;
    return true;
}

bool tpw_filter_refuse_in_callback(const struct tpw_filter* filter, bool loop_thread_too, const char* call)
{
    bool refused = tpw_filter_processing == filter ||
                   (loop_thread_too && filter->conn.loop && pw_thread_loop_in_thread(filter->conn.loop));
    if (refused)
        tpw_log_error("filter '%s': %s() cannot be called from inside this filter's own callbacks",
                      filter->name ? filter->name : "tpw-filter", call);
    return refused;
}

/* Wakes a draining tpw_filter_stop(..., true), waiting on this same flag
 * under filter->conn.loop's lock. */
static void tpw_filter_on_drained(void* data)
{
    struct tpw_filter* filter = data;
    filter->drained = true;
    pw_thread_loop_signal(filter->conn.loop, false);
}

static const struct pw_filter_events tpw_filter_events = {
    PW_VERSION_FILTER_EVENTS,
    .param_changed = tpw_filter_on_param_changed,
    .process = tpw_filter_on_process,
    .drained = tpw_filter_on_drained,
};

bool tpw_filter_add_port_to_list(struct tpw_filter* filter, struct tpw_filter_port* port)
{
    if (filter->n_ports == filter->ports_capacity) {
        size_t new_cap = filter->ports_capacity == 0 ? 4 : filter->ports_capacity * 2;
        struct tpw_filter_port** grown = realloc(filter->ports, new_cap * sizeof(*grown));
        if (!grown)
            return false;
        filter->ports = grown;
        filter->ports_capacity = new_cap;
    }
    filter->ports[filter->n_ports++] = port;
    return true;
}

static void tpw_filter_teardown(struct tpw_filter* filter)
{
    if (!filter)
        return;

    if (filter->conn.loop && filter->pw_filter) {
        pw_thread_loop_lock(filter->conn.loop);
        pw_filter_destroy(filter->pw_filter);
        filter->pw_filter = NULL;
        pw_thread_loop_unlock(filter->conn.loop);
    } else if (filter->pw_filter) {
        pw_filter_destroy(filter->pw_filter);
        filter->pw_filter = NULL;
    }

    tpw_pw_core_teardown(&filter->conn);
}

tpw_filter_h tpw_filter_create(const char* name, tpw_filter_process_cb callback, void* user_data)
{
    if (!callback)
        return NULL;

    tpw_pw_global_init();

    struct tpw_filter* filter = calloc(1, sizeof(*filter));
    if (!filter) {
        tpw_pw_global_deinit();
        return NULL;
    }

    filter->state = TPW_FILTER_STATE_CREATED;
    filter->process_cb = callback;
    filter->user_data = user_data;

    if (name && *name) {
        filter->name = strdup(name);
        if (!filter->name) {
            free(filter);
            tpw_pw_global_deinit();
            return NULL;
        }
    }

    if (tpw_pw_core_connect(&filter->conn, "tpw-filter-loop") < 0) {
        tpw_filter_teardown(filter);
        free(filter->name);
        free(filter);
        tpw_pw_global_deinit();
        return NULL;
    }

    pw_thread_loop_lock(filter->conn.loop);
    /* Linking a port is optional, so ports may stay unlinked forever;
     * node.always-process keeps .process() firing regardless so the app can
     * drive I/O via push_port_data. */
    struct pw_properties* props = pw_properties_new(PW_KEY_NODE_ALWAYS_PROCESS, "true", NULL);
    /* PipeWire names an unnamed node after the process, so the name has to be
     * set here for other nodes and tpw_filter_port_link() to find it. */
    if (props && filter->name)
        pw_properties_set(props, PW_KEY_NODE_NAME, filter->name);
    filter->pw_filter =
        pw_filter_new(filter->conn.core, filter->name ? filter->name : "tpw-filter", props);
    if (!filter->pw_filter) {
        pw_thread_loop_unlock(filter->conn.loop);
        tpw_log_error("filter '%s': failed to create pipewire filter", filter->name ? filter->name : "tpw-filter");
        tpw_filter_teardown(filter);
        free(filter->name);
        free(filter);
        tpw_pw_global_deinit();
        return NULL;
    }

    pw_filter_add_listener(filter->pw_filter, &filter->filter_listener, &tpw_filter_events, filter);
    pw_thread_loop_unlock(filter->conn.loop);

    /* Ports need a created filter, so no push can reach the lock before this. */
    pthread_mutex_init(&filter->push_lock, NULL);
    return (tpw_filter_h)filter;
}

int tpw_filter_set_error_cb(tpw_filter_h handle, tpw_filter_error_cb callback)
{
    struct tpw_filter* filter = (struct tpw_filter*)handle;
    if (!filter)
        return TPW_STREAM_ERR_INVALID_ARG;

    filter->error_cb = callback;
    return TPW_STREAM_OK;
}

int tpw_filter_set_period_hint(tpw_filter_h handle, uint32_t max_period_ns)
{
    struct tpw_filter* filter = (struct tpw_filter*)handle;
    if (!filter)
        return TPW_STREAM_ERR_INVALID_ARG;
    if (filter->state != TPW_FILTER_STATE_CREATED)
        return TPW_STREAM_ERR_INVALID_ARG;

    filter->period_hint_ns = max_period_ns;
    return TPW_STREAM_OK;
}

/* Numerator of the node.latency "num/48000" time ratio for a period hint,
 * floored (never coarser than requested) and clamped to at least 1. The
 * denominator 48000 is an arbitrary reference — PipeWire rescales the ratio
 * to the actual graph clock, so no real sample rate is assumed. */
uint32_t tpw_filter_period_hint_num(uint32_t period_ns)
{
    uint64_t num = (uint64_t)period_ns * 48000u / 1000000000ULL;
    return num < 1 ? 1u : (uint32_t)num;
}

/* Applies the period hint as a node.latency preference; a no-op when unset.
 * PipeWire rescales the ratio to the graph clock. */
static void tpw_filter_apply_period_hint(struct tpw_filter* filter)
{
    if (filter->period_hint_ns == 0)
        return;

    char latency[32];
    snprintf(latency, sizeof(latency), "%u/48000", tpw_filter_period_hint_num(filter->period_hint_ns));

    struct spa_dict_item items[] = { SPA_DICT_ITEM_INIT(PW_KEY_NODE_LATENCY, latency) };
    struct spa_dict dict = SPA_DICT_INIT(items, 1);
    pw_filter_update_properties(filter->pw_filter, NULL, &dict);
}

int tpw_filter_start(tpw_filter_h handle)
{
    struct tpw_filter* filter = (struct tpw_filter*)handle;
    if (!filter)
        return TPW_STREAM_ERR_INVALID_ARG;
    if (tpw_filter_refuse_in_callback(filter, false, __func__))
        return TPW_STREAM_ERR_IN_CALLBACK;
    if (filter->n_ports == 0)
        return TPW_STREAM_ERR_NOT_CONFIGURED;

    pw_thread_loop_lock(filter->conn.loop);
    if (filter->state == TPW_FILTER_STATE_CREATED) {
        /* First start: ports were already added with their format params,
         * so connecting now negotiates and activates them together. The
         * period hint (if any) is a node property, so it must be set before
         * connect. */
        tpw_filter_apply_period_hint(filter);
        int res = pw_filter_connect(filter->pw_filter, PW_FILTER_FLAG_RT_PROCESS, NULL, 0);
        if (res < 0) {
            pw_thread_loop_unlock(filter->conn.loop);
            tpw_log_error("filter '%s': failed to connect (result=%d)", filter->name ? filter->name : "tpw-filter", res);
            return TPW_STREAM_ERR_CONNECT_FAILED;
        }
    } else {
        pw_filter_set_active(filter->pw_filter, true);
    }
    pw_thread_loop_unlock(filter->conn.loop);

    filter->state = TPW_FILTER_STATE_RUNNING;
    return TPW_STREAM_OK;
}

int tpw_filter_stop(tpw_filter_h handle, bool drain)
{
    struct tpw_filter* filter = (struct tpw_filter*)handle;
    if (!filter)
        return TPW_STREAM_ERR_INVALID_ARG;
    if (tpw_filter_refuse_in_callback(filter, drain, __func__))
        return TPW_STREAM_ERR_IN_CALLBACK;
    if (filter->state != TPW_FILTER_STATE_RUNNING)
        return TPW_STREAM_OK;

    pw_thread_loop_lock(filter->conn.loop);

    if (drain) {
        /* Must run before the links are dropped below, or a disconnected
         * output port's queued data has nowhere left to go. */
        struct timespec deadline;
        filter->drained = false;
        pw_thread_loop_get_time(filter->conn.loop, &deadline, TPW_FILTER_DRAIN_TIMEOUT_NSEC);
        pw_filter_flush(filter->pw_filter, true);

        while (!filter->drained) {
            if (pw_thread_loop_timed_wait_full(filter->conn.loop, &deadline) < 0) {
                tpw_log_warning("filter '%s': timed out waiting to drain; stopping anyway",
                                filter->name ? filter->name : "tpw-filter");
                break;
            }
        }
    }

    pw_filter_set_active(filter->pw_filter, false);
    /* Return any held buffer to the pool now that processing is paused, and
     * reset per-port hold state so a restart begins holding afresh. */
    for (size_t i = 0; i < filter->n_ports; i++) {
        struct tpw_filter_port* port = filter->ports[i];
        if (port->held) {
            pw_filter_queue_buffer(port, port->held);
            port->held = NULL;
        }
        port->has_held = false;
        port->held_data = NULL;
        port->held_dmabuf_buf = NULL;
        port->current_dmabuf_buf = NULL;
    }
    /* Set before the links go, so the formats they clear are not reported as lost sources. */
    filter->state = TPW_FILTER_STATE_STOPPED;
    pw_thread_loop_unlock(filter->conn.loop);

    /* Links were made against the running graph and a restart re-links
     * explicitly, so they are dropped only now that processing is paused. */
    tpw_filter_release_all_links(filter);
    return TPW_STREAM_OK;
}

void tpw_filter_destroy(tpw_filter_h handle)
{
    struct tpw_filter* filter = (struct tpw_filter*)handle;
    if (!filter || tpw_filter_refuse_in_callback(filter, true, __func__))
        return;

    if (filter->state == TPW_FILTER_STATE_RUNNING)
        tpw_filter_stop(handle, false);
    else
        tpw_filter_release_all_links(filter);

    /* The registry and any links must go while the loop still runs, since
     * destroying their proxies talks to the server. */
    if (filter->conn.loop) {
        pw_thread_loop_lock(filter->conn.loop);
        tpw_pw_registry_teardown(&filter->registry);
        pw_thread_loop_unlock(filter->conn.loop);
    }

    /* The ports themselves are owned by pw_filter and freed by
     * pw_filter_destroy() inside tpw_filter_teardown(); only the extra
     * heap allocations for each port's push-staging buffer/events are
     * ours, so they must be freed before teardown while the port
     * structs are still valid. */
    for (size_t i = 0; i < filter->n_ports; i++) {
        if (filter->ports[i]) {
            free(filter->ports[i]->pushed_data);
            free(filter->ports[i]->delivered_data);
            tpw_filter_event_free_port(filter->ports[i]);
        }
    }
    free(filter->ports);

    tpw_filter_teardown(filter);

    pthread_mutex_destroy(&filter->push_lock);
    free(filter->name);
    free(filter);
    tpw_pw_global_deinit();
}
