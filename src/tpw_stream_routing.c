/* SPDX-License-Identifier: MIT */

#include <stdlib.h>
#include <string.h>

#include "tpw_log_internal.h"
#include "tpw_stream_internal.h"

/* Matches the core connect timeout, as the filter's link does. */
#define TPW_LINK_TIMEOUT_NSEC (5 * SPA_NSEC_PER_SEC)

/* The server publishes a stream's ports shortly after it connects — tens of
 * milliseconds in practice. Waiting is required; how long is not interesting. */
#define TPW_OWN_PORTS_TIMEOUT_NSEC (2 * SPA_NSEC_PER_SEC)

#define TPW_MAX_CHANNELS 64

size_t tpw_stream_pair_ports(size_t n_stream, size_t n_target, size_t* surplus)
{
    if (surplus)
        *surplus = 0;
    if (n_stream == 0 || n_target < n_stream)
        return 0; /* a stream never ends up half-wired */

    if (surplus)
        *surplus = n_target - n_stream;
    return n_stream;
}

/* --- waiting for our own ports --------------------------------------- */

static void tpw_stream_on_port_added(void* data, uint32_t node_id)
{
    struct tpw_stream* stream = data;
    (void)node_id;
    pw_thread_loop_signal(stream->conn.loop, false);
}

static void tpw_stream_on_node_removed(void* data, uint32_t id)
{
    struct tpw_stream* stream = data;

    if (!stream->links || stream->links->target_node_id != id)
        return;

    tpw_log_warning("stream: linked device disappeared");
    tpw_stream_release_links(stream);
    if (stream->error_cb)
        stream->error_cb((tpw_stream_h)stream, TPW_STREAM_ERR_SOURCE_UNAVAILABLE, stream->user_data);
}

/* How many ports this stream should have once the server has published them. */
static size_t tpw_stream_expected_ports(const struct tpw_stream* stream)
{
    return stream->type == TPW_STREAM_TYPE_AUDIO && stream->format.audio.channels > 0
               ? (size_t)stream->format.audio.channels
               : 1;
}

/* The direction our own ports face: a capture stream reads through inputs,
 * a playback stream writes through outputs. */
static enum spa_direction tpw_stream_own_direction(const struct tpw_stream* stream)
{
    return stream->direction == TPW_STREAM_DIRECTION_PLAYBACK ? SPA_DIRECTION_OUTPUT : SPA_DIRECTION_INPUT;
}

/* Blocks until this stream has a node id and the registry has cached every
 * port it expects, woken by the state-changed and port-added callbacks rather
 * than by polling. Both arrive after start: the id when the server has seen
 * the node, the ports a little after that. */
static int tpw_stream_await_own_ports(struct tpw_stream* stream, size_t expected, uint32_t* out_node_id)
{
    struct timespec deadline;
    enum spa_direction dir = tpw_stream_own_direction(stream);
    uint32_t node_id;

    pw_thread_loop_lock(stream->conn.loop);
    pw_thread_loop_get_time(stream->conn.loop, &deadline, TPW_OWN_PORTS_TIMEOUT_NSEC);

    for (;;) {
        node_id = pw_stream_get_node_id(stream->pw_stream);
        if (node_id != SPA_ID_INVALID &&
            tpw_pw_registry_list_ports(&stream->registry, node_id, dir, NULL, 0) >= expected)
            break;

        if (pw_thread_loop_timed_wait_full(stream->conn.loop, &deadline) < 0) {
            pw_thread_loop_unlock(stream->conn.loop);
            tpw_log_error("stream: timed out waiting for this stream to appear in the graph");
            return TPW_STREAM_ERR_TIMEOUT;
        }
    }

    pw_thread_loop_unlock(stream->conn.loop);
    *out_node_id = node_id;
    return TPW_STREAM_OK;
}

/* --- target resolution ------------------------------------------------ */

static bool tpw_str_is_all_digits(const char* s)
{
    if (!s || !*s)
        return false;
    for (const char* p = s; *p; p++)
        if (*p < '0' || *p > '9')
            return false;
    return true;
}

static uint32_t tpw_stream_resolve_target(struct tpw_stream* stream, const char* target)
{
    return tpw_str_is_all_digits(target)
               ? tpw_pw_registry_find_node_by_serial(&stream->registry, strtoull(target, NULL, 10))
               : tpw_pw_registry_find_node_by_name(&stream->registry, target);
}

/* The media.class `stream` should list targets for: sinks for playback,
 * sources for its own audio/video type otherwise. */
static const char* tpw_stream_target_media_class(const struct tpw_stream* stream)
{
    if (stream->direction == TPW_STREAM_DIRECTION_PLAYBACK)
        return "Audio/Sink";
    return stream->type == TPW_STREAM_TYPE_VIDEO ? "Video/Source" : "Audio/Source";
}

int tpw_stream_get_target_list(tpw_stream_h handle, tpw_target_info* out, size_t out_len,
                                size_t* found)
{
    struct tpw_stream* stream = (struct tpw_stream*)handle;
    if (!found)
        return TPW_STREAM_ERR_INVALID_ARG;

    *found = 0;
    if (!stream)
        return TPW_STREAM_ERR_INVALID_ARG;
    if (tpw_stream_refuse_in_callback(stream, true, __func__))
        return TPW_STREAM_ERR_IN_CALLBACK;
    int bound = tpw_pw_registry_bind(&stream->registry, &stream->conn);
    if (bound < 0)
        return tpw_pw_error_from_sync(bound);

    const char* media_class = tpw_stream_target_media_class(stream);

    pw_thread_loop_lock(stream->conn.loop);
    size_t n = 0;
    for (size_t i = 0; i < stream->registry.n_nodes; i++) {
        const struct tpw_pw_node_entry* e = &stream->registry.nodes[i];
        if (!e->media_class || strcmp(e->media_class, media_class) != 0)
            continue;
        if (out && n < out_len) {
            snprintf(out[n].name, sizeof(out[n].name), "%s", e->name);
            snprintf(out[n].description, sizeof(out[n].description), "%s",
                      e->description ? e->description : "");
            snprintf(out[n].serial, sizeof(out[n].serial), "%llu", (unsigned long long)e->serial);
        }
        n++;
    }
    pw_thread_loop_unlock(stream->conn.loop);

    *found = n;
    return TPW_STREAM_OK;
}

int tpw_stream_get_target_video_formats(tpw_stream_h handle, const char* target,
                                         tpw_video_format_info* out, size_t out_len,
                                         size_t* found)
{
    struct tpw_stream* stream = (struct tpw_stream*)handle;
    if (!found)
        return TPW_STREAM_ERR_INVALID_ARG;

    *found = 0;
    if (!stream || stream->type != TPW_STREAM_TYPE_VIDEO)
        return TPW_STREAM_ERR_INVALID_ARG;
    if (tpw_stream_refuse_in_callback(stream, true, __func__))
        return TPW_STREAM_ERR_IN_CALLBACK;

    /* Fall back to the target already set, which is the pairing this call
     * exists for: pick a device, then ask what it can deliver. */
    const char* name = target ? target : stream->target;
    if (!name)
        return TPW_STREAM_ERR_INVALID_ARG;
    int bound = tpw_pw_registry_bind(&stream->registry, &stream->conn);
    if (bound < 0)
        return tpw_pw_error_from_sync(bound);

    uint32_t node_id = tpw_stream_resolve_target(stream, name);
    if (!node_id) {
        tpw_log_warning("stream: no node named '%s' to read formats from", name);
        return TPW_STREAM_ERR_NOT_FOUND;
    }

    return tpw_pw_enum_video_formats(&stream->conn, &stream->registry, node_id, out, out_len, found);
}

/* --- link lifecycle --------------------------------------------------- */

static void tpw_stream_link_on_info(void* data, const struct pw_link_info* info)
{
    struct tpw_stream_link* link = data;

    if (!(info->change_mask & PW_LINK_CHANGE_MASK_STATE))
        return;

    if (info->state == PW_LINK_STATE_ACTIVE || info->state == PW_LINK_STATE_PAUSED)
        link->seen_active = true;
    else if (info->state == PW_LINK_STATE_ERROR)
        link->lost = true;

    pw_thread_loop_signal(link->stream->conn.loop, false);
}

static const struct pw_link_events tpw_stream_link_events = {
    PW_VERSION_LINK_EVENTS,
    .info = tpw_stream_link_on_info,
};

void tpw_stream_release_links(struct tpw_stream* stream)
{
    if (!stream || !stream->links)
        return;

    struct tpw_stream_link_set* set = stream->links;
    stream->links = NULL; /* cleared first: destroying a proxy can re-enter */

    pw_thread_loop_lock(stream->conn.loop);
    for (size_t i = 0; i < set->n_links; i++) {
        if (!set->links[i].proxy)
            continue;
        spa_hook_remove(&set->links[i].listener);
        pw_proxy_destroy(set->links[i].proxy);
    }
    pw_thread_loop_unlock(stream->conn.loop);

    free(set->links);
    free(set);
}

/* Creates one link and waits, bounded, for it to negotiate. */
static int tpw_stream_link_one(struct tpw_stream* stream, struct tpw_stream_link* link,
                                uint32_t out_node, uint32_t out_port, uint32_t in_node, uint32_t in_port)
{
    char on[16], op[16], in[16], ip[16];
    snprintf(on, sizeof(on), "%u", out_node);
    snprintf(op, sizeof(op), "%u", out_port);
    snprintf(in, sizeof(in), "%u", in_node);
    snprintf(ip, sizeof(ip), "%u", in_port);

    struct pw_properties* props =
        pw_properties_new(PW_KEY_LINK_OUTPUT_NODE, on, PW_KEY_LINK_OUTPUT_PORT, op,
                          PW_KEY_LINK_INPUT_NODE, in, PW_KEY_LINK_INPUT_PORT, ip, NULL);
    if (!props)
        return TPW_STREAM_ERR_CONNECT_FAILED;

    link->stream = stream;
    link->seen_active = false;
    link->lost = false;

    pw_thread_loop_lock(stream->conn.loop);

    link->proxy = pw_core_create_object(stream->conn.core, "link-factory", PW_TYPE_INTERFACE_Link,
                                         PW_VERSION_LINK, &props->dict, 0);
    if (!link->proxy) {
        pw_thread_loop_unlock(stream->conn.loop);
        pw_properties_free(props);
        return TPW_STREAM_ERR_CONNECT_FAILED;
    }
    pw_proxy_add_object_listener(link->proxy, &link->listener, &tpw_stream_link_events, link);

    struct timespec deadline;
    pw_thread_loop_get_time(stream->conn.loop, &deadline, TPW_LINK_TIMEOUT_NSEC);

    int res = TPW_STREAM_OK;
    while (!link->seen_active && !link->lost) {
        if (pw_thread_loop_timed_wait_full(stream->conn.loop, &deadline) < 0) {
            res = TPW_STREAM_ERR_TIMEOUT;
            break;
        }
    }
    if (link->lost)
        res = TPW_STREAM_ERR_INVALID_FORMAT;

    pw_thread_loop_unlock(stream->conn.loop);
    pw_properties_free(props);
    return res;
}

int tpw_stream_link(tpw_stream_h handle, const char* target)
{
    struct tpw_stream* stream = (struct tpw_stream*)handle;
    if (!stream || !target || !*target)
        return TPW_STREAM_ERR_INVALID_ARG;
    if (tpw_stream_refuse_in_callback(stream, true, __func__))
        return TPW_STREAM_ERR_IN_CALLBACK;
    if (stream->autoconnect)
        return TPW_STREAM_ERR_INVALID_ARG; /* two parties would own the wiring */
    if (stream->links)
        return TPW_STREAM_ERR_INVALID_ARG; /* unlink first to re-target */
    if (stream->state != TPW_STREAM_STATE_RUNNING || !stream->pw_stream)
        return TPW_STREAM_ERR_NOT_CONFIGURED; /* the graph is where we look things up */

    int bound = tpw_pw_registry_bind(&stream->registry, &stream->conn);
    if (bound < 0)
        return tpw_pw_error_from_sync(bound);
    stream->registry.port_added_cb = tpw_stream_on_port_added;
    stream->registry.port_added_data = stream;
    stream->registry.node_removed_cb = tpw_stream_on_node_removed;
    stream->registry.node_removed_data = stream;

    uint32_t own_node = SPA_ID_INVALID;
    int res = tpw_stream_await_own_ports(stream, tpw_stream_expected_ports(stream), &own_node);
    if (res < 0)
        return res;

    uint32_t target_node = tpw_stream_resolve_target(stream, target);
    if (!target_node) {
        tpw_log_error("stream: no node named '%s' in the graph", target);
        return TPW_STREAM_ERR_NOT_FOUND;
    }

    /* Monitor ports sit in the opposite direction on both nodes and reuse the
     * same ordinals, so direction is part of the key, not a filter. */
    enum spa_direction own_dir = tpw_stream_own_direction(stream);
    bool we_output = own_dir == SPA_DIRECTION_OUTPUT;
    enum spa_direction peer_dir = we_output ? SPA_DIRECTION_INPUT : SPA_DIRECTION_OUTPUT;

    const struct tpw_pw_port_entry* own[TPW_MAX_CHANNELS];
    const struct tpw_pw_port_entry* peer[TPW_MAX_CHANNELS];
    size_t n_own = tpw_pw_registry_list_ports(&stream->registry, own_node, own_dir, own, TPW_MAX_CHANNELS);
    size_t n_peer =
        tpw_pw_registry_list_ports(&stream->registry, target_node, peer_dir, peer, TPW_MAX_CHANNELS);
    if (n_own > TPW_MAX_CHANNELS || n_peer > TPW_MAX_CHANNELS)
        return TPW_STREAM_ERR_INVALID_ARG;

    size_t surplus = 0;
    size_t pairs = tpw_stream_pair_ports(n_own, n_peer, &surplus);
    if (pairs == 0) {
        tpw_log_error("stream: '%s' offers %zu channels, this stream needs %zu", target, n_peer, n_own);
        return TPW_STREAM_ERR_INVALID_ARG;
    }

    struct tpw_stream_link_set* set = calloc(1, sizeof(*set));
    if (!set)
        return TPW_STREAM_ERR_CONNECT_FAILED;
    set->links = calloc(pairs, sizeof(*set->links));
    if (!set->links) {
        free(set);
        return TPW_STREAM_ERR_CONNECT_FAILED;
    }
    set->target_node_id = target_node;
    stream->links = set;

    for (size_t i = 0; i < pairs; i++) {
        res = tpw_stream_link_one(stream, &set->links[i], we_output ? own_node : target_node,
                                   we_output ? own[i]->id : peer[i]->id, we_output ? target_node : own_node,
                                   we_output ? peer[i]->id : own[i]->id);
        set->n_links = i + 1;
        if (res < 0) {
            /* All or nothing: drop whatever this request already made. */
            tpw_stream_release_links(stream);
            tpw_log_error("stream: linking channel %zu to '%s' failed; released the rest", i, target);
            return res;
        }
        tpw_log_info("stream: linked channel %zu -> %s", i, peer[i]->name);
    }

    if (surplus > 0)
        tpw_log_warning("stream: '%s' has %zu channel(s) this stream does not reach, from %s on",
                        target, surplus, peer[pairs]->name);

    return TPW_STREAM_OK;
}

int tpw_stream_unlink(tpw_stream_h handle)
{
    struct tpw_stream* stream = (struct tpw_stream*)handle;
    if (!stream)
        return TPW_STREAM_ERR_INVALID_ARG;
    if (tpw_stream_refuse_in_callback(stream, false, __func__))
        return TPW_STREAM_ERR_IN_CALLBACK;
    if (!stream->links)
        return TPW_STREAM_ERR_INVALID_ARG;

    tpw_stream_release_links(stream);
    return TPW_STREAM_OK;
}
