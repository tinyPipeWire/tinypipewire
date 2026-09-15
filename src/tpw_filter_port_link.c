/* SPDX-License-Identifier: MIT */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pipewire/keys.h>
#include <pipewire/link.h>

#include "tpw_filter_internal.h"
#include "tpw_log_internal.h"

/* How long tpw_filter_port_link() waits for a link to finish negotiating
 * before giving up, matching the core connect timeout. */
#define TPW_LINK_TIMEOUT_NSEC (5 * SPA_NSEC_PER_SEC)

/* How long to keep re-checking the registry for this filter's own port,
 * which the server publishes some time after connect (100 x 20ms = 2s). */
#define TPW_OWN_PORT_ATTEMPTS   100
#define TPW_OWN_PORT_RETRY_USEC (20 * 1000)

/* How many core round-trips to wait for a target node's ports to reach the
 * registry before linking without naming one. */
#define TPW_TARGET_PORT_ATTEMPTS 10

/* Upper bound on the output ports considered when picking one; more than
 * this on a single capture node does not happen in practice. */
#define TPW_MAX_TARGET_PORTS 64

/* Tracks one in-flight link's negotiation while tpw_filter_port_link()
 * blocks on the thread loop. */
struct tpw_link_wait {
    struct tpw_filter_port* port;
    bool done;
    int result;
};

static bool tpw_str_is_all_digits(const char* s)
{
    if (!*s)
        return false;
    for (const char* p = s; *p; p++) {
        if (*p < '0' || *p > '9')
            return false;
    }
    return true;
}

/* Picks the lowest-ordinal output port, so every consumer naming the same
 * node lands on it; the core would instead hand out whichever port is
 * unlinked. Returns 0 if the ports have not arrived, leaving the core to pick. */
static uint32_t tpw_pick_target_port(struct tpw_filter* filter, uint32_t node_id)
{
    const struct tpw_pw_port_entry* outs[TPW_MAX_TARGET_PORTS];

    for (int attempt = 0; attempt < TPW_TARGET_PORT_ATTEMPTS; attempt++) {
        size_t n = tpw_pw_registry_list_ports(&filter->registry, node_id, SPA_DIRECTION_OUTPUT, outs,
                                               TPW_MAX_TARGET_PORTS);
        if (n > 0) {
            if (n > 1)
                tpw_log_debug("filter '%s': target has %zu output ports, linking to '%s'; name it as "
                              "node:port to choose another",
                              filter->name ? filter->name : "tpw-filter", n, outs[0]->name);
            return outs[0]->id;
        }
        if (tpw_pw_registry_sync(&filter->registry, &filter->conn) < 0)
            break;
    }
    return 0;
}

/* Resolves `target` to a node id alone, setting `*out_port_name` to the port
 * part or NULL; split out so a format query costs no port lookup. 0 if none. */
static uint32_t tpw_resolve_target_node(struct tpw_filter* filter, const char* target,
                                         const char** out_port_name)
{
    struct tpw_pw_registry* reg = &filter->registry;
    *out_port_name = NULL;

    /* Try the whole string first, so a node whose name contains ':'
     * still resolves before we treat ':' as a separator. */
    uint32_t node_id = tpw_str_is_all_digits(target)
                           ? tpw_pw_registry_find_node_by_serial(reg, strtoull(target, NULL, 10))
                           : tpw_pw_registry_find_node_by_name(reg, target);
    if (node_id)
        return node_id;

    const char* sep = strrchr(target, ':');
    if (!sep || sep == target || !sep[1])
        return 0;

    char node_part[256];
    size_t node_len = (size_t)(sep - target);
    if (node_len >= sizeof(node_part))
        return 0;
    memcpy(node_part, target, node_len);
    node_part[node_len] = '\0';

    node_id = tpw_str_is_all_digits(node_part)
                  ? tpw_pw_registry_find_node_by_serial(reg, strtoull(node_part, NULL, 10))
                  : tpw_pw_registry_find_node_by_name(reg, node_part);
    if (!node_id)
        return 0;

    *out_port_name = sep + 1;
    return node_id;
}

/* Resolves `target` to a node id, and to the output port id to link, whether
 * the target names one explicitly or not. Returns 0 when nothing matches. */
static uint32_t tpw_resolve_target(struct tpw_filter* filter, const char* target, uint32_t* out_port_id)
{
    const char* port_name = NULL;
    *out_port_id = 0;

    uint32_t node_id = tpw_resolve_target_node(filter, target, &port_name);
    if (!node_id)
        return 0;

    if (!port_name) {
        *out_port_id = tpw_pick_target_port(filter, node_id);
        return node_id;
    }

    uint32_t port_id =
        tpw_pw_registry_find_port(&filter->registry, node_id, port_name, SPA_DIRECTION_OUTPUT);
    if (!port_id)
        return 0;

    *out_port_id = port_id;
    return node_id;
}

int tpw_filter_get_target_video_formats(tpw_filter_h handle, const char* target,
                                         tpw_video_format_info* out, size_t out_len,
                                         size_t* found)
{
    struct tpw_filter* filter = (struct tpw_filter*)handle;
    if (!found)
        return TPW_STREAM_ERR_INVALID_ARG;

    *found = 0;
    if (!filter || !target)
        return TPW_STREAM_ERR_INVALID_ARG;
    if (tpw_filter_refuse_in_callback(filter, true, __func__))
        return TPW_STREAM_ERR_IN_CALLBACK;
    int bound = tpw_pw_registry_bind(&filter->registry, &filter->conn);
    if (bound < 0)
        return tpw_pw_error_from_sync(bound);

    /* Formats belong to the node, so a named port only helps find it. */
    const char* port_name = NULL;
    uint32_t node_id = tpw_resolve_target_node(filter, target, &port_name);
    if (!node_id) {
        tpw_log_warning("filter: no node named '%s' to read formats from", target);
        return TPW_STREAM_ERR_NOT_FOUND;
    }

    return tpw_pw_enum_video_formats(&filter->conn, &filter->registry, node_id, out, out_len, found);
}

/* Finds this port's own global id by the name it was created with. The
 * filter's node and ports are published asynchronously after connect, so
 * this retries across core round-trips until they show up. */
static uint32_t tpw_resolve_own_port_id(struct tpw_filter_port* port)
{
    if (port->own_global_id)
        return port->own_global_id;

    struct tpw_filter* filter = port->filter;
    for (int attempt = 0; attempt < TPW_OWN_PORT_ATTEMPTS; attempt++) {
        uint32_t node_id = pw_filter_get_node_id(filter->pw_filter);
        if (node_id != SPA_ID_INVALID) {
            port->own_global_id =
                tpw_pw_registry_find_port(&filter->registry, node_id, port->pw_port_name, SPA_DIRECTION_INPUT);
            if (port->own_global_id)
                return port->own_global_id;
        }
        usleep(TPW_OWN_PORT_RETRY_USEC);
        if (tpw_pw_registry_sync(&filter->registry, &filter->conn) < 0)
            break;
    }
    return 0;
}

void tpw_filter_link_on_info(void* data, const struct pw_link_info* info)
{
    struct tpw_filter_port* port = data;
    struct tpw_filter* filter = port->filter;

    if (!(info->change_mask & PW_LINK_CHANGE_MASK_STATE))
        return;

    if (info->state == PW_LINK_STATE_PAUSED || info->state == PW_LINK_STATE_ACTIVE) {
        port->link_state_seen_active = true;
        if (port->link_wait) {
            struct tpw_link_wait* wait = port->link_wait;
            wait->done = true;
            wait->result = TPW_STREAM_OK;
            pw_thread_loop_signal(filter->conn.loop, false);
        }
        return;
    }

    if (info->state != PW_LINK_STATE_ERROR && info->state != PW_LINK_STATE_UNLINKED)
        return;

    if (port->link_wait) {
        /* Still negotiating: fail the pending tpw_filter_port_link() call. */
        struct tpw_link_wait* wait = port->link_wait;
        wait->done = true;
        wait->result = TPW_STREAM_ERR_INVALID_FORMAT;
        pw_thread_loop_signal(filter->conn.loop, false);
        return;
    }

    if (!port->link_state_seen_active)
        return;

    /* The link was up and has now gone away — the source is gone. Only the
     * port's state is cleared here; destroying the proxy from inside its own
     * callback would be a use-after-free, so the owner does it later. */
    port->link_state_seen_active = false;
    port->link_lost = true;
    if (!tpw_filter_claim_source_loss(filter, port))
        return;
    tpw_log_warning("filter '%s': a linked source became unavailable",
                    filter->name ? filter->name : "tpw-filter");
    if (filter->error_cb)
        filter->error_cb((tpw_filter_h)filter, (tpw_filter_port_h)port, TPW_STREAM_ERR_SOURCE_UNAVAILABLE,
                          filter->user_data);
}

static const struct pw_link_events tpw_filter_link_events = {
    PW_VERSION_LINK_EVENTS,
    .info = tpw_filter_link_on_info,
};

/* Destroys `port`'s link proxy and clears its link state. The thread loop
 * must already be locked. */
static void tpw_filter_port_link_release(struct tpw_filter_port* port)
{
    if (port->link_proxy) {
        spa_hook_remove(&port->link_listener);
        pw_proxy_destroy(port->link_proxy);
        port->link_proxy = NULL;
        /* The application dropped this link, so the format it clears is not a lost source. */
        port->loss_reported = true;
    }
    port->link_state_seen_active = false;
    port->link_lost = false;
}

int tpw_filter_port_link(tpw_filter_port_h port_handle, const char* target)
{
    struct tpw_filter_port* port = (struct tpw_filter_port*)port_handle;
    if (!port || !target || !*target)
        return TPW_STREAM_ERR_INVALID_ARG;
    if (port->direction != TPW_FILTER_PORT_INPUT)
        return TPW_STREAM_ERR_INVALID_ARG;
    if (tpw_filter_refuse_in_callback(port->filter, true, __func__))
        return TPW_STREAM_ERR_IN_CALLBACK;
    if (port->link_proxy)
        return TPW_STREAM_ERR_INVALID_ARG;

    struct tpw_filter* filter = port->filter;
    if (!filter || filter->state != TPW_FILTER_STATE_RUNNING)
        return TPW_STREAM_ERR_NOT_CONFIGURED;

    int bound = tpw_pw_registry_bind(&filter->registry, &filter->conn);
    if (bound < 0)
        return tpw_pw_error_from_sync(bound);

    uint32_t target_port_id = 0;
    uint32_t target_node_id = tpw_resolve_target(filter, target, &target_port_id);
    if (!target_node_id) {
        tpw_log_warning("filter '%s': no pipewire node matches target '%s'",
                        filter->name ? filter->name : "tpw-filter", target);
        return TPW_STREAM_ERR_NOT_FOUND;
    }

    uint32_t own_port_id = tpw_resolve_own_port_id(port);
    if (!own_port_id) {
        tpw_log_warning("filter '%s': could not find this port in the pipewire registry",
                        filter->name ? filter->name : "tpw-filter");
        return TPW_STREAM_ERR_CONNECT_FAILED;
    }

    char in_node[16], in_port[16], out_node[16], out_port[16];
    snprintf(in_node, sizeof(in_node), "%u", pw_filter_get_node_id(filter->pw_filter));
    snprintf(in_port, sizeof(in_port), "%u", own_port_id);
    snprintf(out_node, sizeof(out_node), "%u", target_node_id);

    struct pw_properties* props = pw_properties_new(PW_KEY_LINK_INPUT_NODE, in_node,
                                                     PW_KEY_LINK_INPUT_PORT, in_port,
                                                     PW_KEY_LINK_OUTPUT_NODE, out_node, NULL);
    if (!props)
        return TPW_STREAM_ERR_CONNECT_FAILED;
    /* Only unset when the target's ports have not shown up yet; then the
     * core's link factory picks one on the target node. */
    if (target_port_id) {
        snprintf(out_port, sizeof(out_port), "%u", target_port_id);
        pw_properties_set(props, PW_KEY_LINK_OUTPUT_PORT, out_port);
    }

    struct tpw_link_wait wait = { .port = port, .done = false, .result = TPW_STREAM_ERR_CONNECT_FAILED };

    pw_thread_loop_lock(filter->conn.loop);

    struct pw_proxy* proxy = pw_core_create_object(filter->conn.core, "link-factory", PW_TYPE_INTERFACE_Link,
                                                    PW_VERSION_LINK, &props->dict, 0);
    if (!proxy) {
        pw_thread_loop_unlock(filter->conn.loop);
        pw_properties_free(props);
        tpw_log_error("filter '%s': failed to create a link to '%s'",
                      filter->name ? filter->name : "tpw-filter", target);
        return TPW_STREAM_ERR_CONNECT_FAILED;
    }

    port->link_proxy = proxy;
    port->link_state_seen_active = false;
    port->link_lost = false;
    port->link_wait = &wait;
    pw_proxy_add_object_listener(proxy, &port->link_listener, &tpw_filter_link_events, port);

    struct timespec deadline;
    pw_thread_loop_get_time(filter->conn.loop, &deadline, TPW_LINK_TIMEOUT_NSEC);
    while (!wait.done) {
        if (pw_thread_loop_timed_wait_full(filter->conn.loop, &deadline) < 0) {
            wait.result = TPW_STREAM_ERR_TIMEOUT;
            break;
        }
    }
    port->link_wait = NULL;

    int result = wait.done ? wait.result : TPW_STREAM_ERR_TIMEOUT;
    if (result != TPW_STREAM_OK)
        tpw_filter_port_link_release(port);

    pw_thread_loop_unlock(filter->conn.loop);
    pw_properties_free(props);

    if (result != TPW_STREAM_OK)
        tpw_log_warning("filter '%s': link to '%s' did not negotiate (result=%d)",
                        filter->name ? filter->name : "tpw-filter", target, result);
    return result;
}

int tpw_filter_port_unlink(tpw_filter_port_h port_handle)
{
    struct tpw_filter_port* port = (struct tpw_filter_port*)port_handle;
    if (!port || port->direction != TPW_FILTER_PORT_INPUT)
        return TPW_STREAM_ERR_INVALID_ARG;
    if (tpw_filter_refuse_in_callback(port->filter, false, __func__))
        return TPW_STREAM_ERR_IN_CALLBACK;
    if (!port->link_proxy)
        return TPW_STREAM_ERR_NOT_CONFIGURED;

    struct tpw_filter* filter = port->filter;
    pw_thread_loop_lock(filter->conn.loop);
    tpw_filter_port_link_release(port);
    pw_thread_loop_unlock(filter->conn.loop);
    return TPW_STREAM_OK;
}

void tpw_filter_release_all_links(struct tpw_filter* filter)
{
    if (!filter || !filter->conn.loop)
        return;

    pw_thread_loop_lock(filter->conn.loop);
    for (size_t i = 0; i < filter->n_ports; i++) {
        if (filter->ports[i])
            tpw_filter_port_link_release(filter->ports[i]);
    }
    pw_thread_loop_unlock(filter->conn.loop);
}
