/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <pipewire/keys.h>

#include "tpw_log_internal.h"
#include "tpw_pw_core_internal.h"

/* How long tpw_pw_core_connect() waits for PipeWire to confirm the
 * connection before failing fast instead of blocking indefinitely. */
#define TPW_CONNECT_TIMEOUT_NSEC (5 * SPA_NSEC_PER_SEC)

static pthread_mutex_t g_pw_init_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_pw_init_count = 0;

void tpw_pw_global_init(void)
{
    pthread_mutex_lock(&g_pw_init_mutex);
    if (g_pw_init_count == 0)
        pw_init(NULL, NULL);
    g_pw_init_count++;
    pthread_mutex_unlock(&g_pw_init_mutex);
}

void tpw_pw_global_deinit(void)
{
    pthread_mutex_lock(&g_pw_init_mutex);
    g_pw_init_count--;
    if (g_pw_init_count == 0)
        pw_deinit();
    pthread_mutex_unlock(&g_pw_init_mutex);
}

static void tpw_pw_core_on_done(void* data, uint32_t id, int seq)
{
    struct tpw_pw_core_conn* conn = data;
    if (id == PW_ID_CORE && seq == conn->pending_seq) {
        conn->sync_done = true;
        pw_thread_loop_signal(conn->loop, false);
    }
}

static void tpw_pw_core_on_error(void* data, uint32_t id, int seq, int res, const char* message)
{
    struct tpw_pw_core_conn* conn = data;
    (void)id;
    (void)seq;
    (void)message;
    conn->connect_result = (res < 0) ? res : -1;
    conn->sync_done = true;
    pw_thread_loop_signal(conn->loop, false);
}

static const struct pw_core_events tpw_pw_core_events = {
    PW_VERSION_CORE_EVENTS,
    .done = tpw_pw_core_on_done,
    .error = tpw_pw_core_on_error,
};

int tpw_pw_core_connect(struct tpw_pw_core_conn* conn, const char* loop_name)
{
    conn->loop = pw_thread_loop_new(loop_name, NULL);
    if (!conn->loop) {
        tpw_log_error("'%s': failed to create thread loop", loop_name);
        return -1;
    }

    if (pw_thread_loop_start(conn->loop) < 0) {
        tpw_log_error("'%s': failed to start thread loop", loop_name);
        return -1;
    }

    pw_thread_loop_lock(conn->loop);

    conn->context = pw_context_new(pw_thread_loop_get_loop(conn->loop), NULL, 0);
    if (!conn->context) {
        pw_thread_loop_unlock(conn->loop);
        tpw_log_error("'%s': failed to create pipewire context", loop_name);
        return -1;
    }

    conn->core = pw_context_connect(conn->context, NULL, 0);
    if (!conn->core) {
        pw_thread_loop_unlock(conn->loop);
        tpw_log_error("'%s': failed to connect to pipewire core", loop_name);
        return -1;
    }

    pw_core_add_listener(conn->core, &conn->core_listener, &tpw_pw_core_events, conn);

    conn->pending_seq = pw_core_sync(conn->core, PW_ID_CORE, 0);
    conn->sync_done = false;
    conn->connect_result = 0;

    struct timespec deadline;
    pw_thread_loop_get_time(conn->loop, &deadline, TPW_CONNECT_TIMEOUT_NSEC);
    while (!conn->sync_done) {
        if (pw_thread_loop_timed_wait_full(conn->loop, &deadline) < 0) {
            conn->connect_result = -1;
            break;
        }
    }

    spa_hook_remove(&conn->core_listener);
    pw_thread_loop_unlock(conn->loop);

    if (conn->connect_result < 0)
        tpw_log_error("'%s': pipewire core sync failed or timed out (result=%d)", loop_name, conn->connect_result);

    return conn->connect_result;
}

void tpw_pw_core_teardown(struct tpw_pw_core_conn* conn)
{
    if (!conn)
        return;

    /* Stop the loop thread before destroying resources so no callback
     * races with teardown; afterwards destruction needs no locking. */
    if (conn->loop)
        pw_thread_loop_stop(conn->loop);

    if (conn->core) {
        pw_core_disconnect(conn->core);
        conn->core = NULL;
    }
    if (conn->context) {
        pw_context_destroy(conn->context);
        conn->context = NULL;
    }
    if (conn->loop) {
        pw_thread_loop_destroy(conn->loop);
        conn->loop = NULL;
    }
}

static void tpw_pw_registry_add_node(struct tpw_pw_registry* reg, uint32_t id, const struct spa_dict* props)
{
    const char* name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    const char* serial = spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL);
    const char* media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    const char* description = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
    if (!name)
        return;

    if (reg->n_nodes == reg->nodes_capacity) {
        size_t cap = reg->nodes_capacity == 0 ? 16 : reg->nodes_capacity * 2;
        struct tpw_pw_node_entry* grown = realloc(reg->nodes, cap * sizeof(*grown));
        if (!grown)
            return;
        reg->nodes = grown;
        reg->nodes_capacity = cap;
    }

    struct tpw_pw_node_entry* e = &reg->nodes[reg->n_nodes];
    e->name = strdup(name);
    if (!e->name)
        return;
    e->id = id;
    e->serial = serial ? strtoull(serial, NULL, 10) : 0;
    e->media_class = media_class ? strdup(media_class) : NULL;
    e->description = description ? strdup(description) : NULL;
    reg->n_nodes++;
}

static void tpw_pw_registry_add_port(struct tpw_pw_registry* reg, uint32_t id, const struct spa_dict* props)
{
    const char* name = spa_dict_lookup(props, PW_KEY_PORT_NAME);
    const char* node_id = spa_dict_lookup(props, PW_KEY_NODE_ID);
    const char* dir = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION);
    const char* ordinal = spa_dict_lookup(props, PW_KEY_PORT_ID);
    if (!name || !node_id)
        return;

    if (reg->n_ports == reg->ports_capacity) {
        size_t cap = reg->ports_capacity == 0 ? 32 : reg->ports_capacity * 2;
        struct tpw_pw_port_entry* grown = realloc(reg->ports, cap * sizeof(*grown));
        if (!grown)
            return;
        reg->ports = grown;
        reg->ports_capacity = cap;
    }

    struct tpw_pw_port_entry* e = &reg->ports[reg->n_ports];
    e->name = strdup(name);
    if (!e->name)
        return;
    e->id = id;
    e->node_id = (uint32_t)strtoul(node_id, NULL, 10);
    /* PipeWire names a port's direction from the port's own point of view:
     * "out" is a source that can feed someone else's input. */
    e->direction = (dir && strcmp(dir, "out") == 0) ? SPA_DIRECTION_OUTPUT : SPA_DIRECTION_INPUT;
    e->ordinal = ordinal ? (uint32_t)strtoul(ordinal, NULL, 10) : 0;
    reg->n_ports++;

    if (reg->port_added_cb)
        reg->port_added_cb(reg->port_added_data, e->node_id);
}

static void tpw_pw_registry_on_global(void* data, uint32_t id, uint32_t permissions, const char* type,
                                       uint32_t version, const struct spa_dict* props)
{
    struct tpw_pw_registry* reg = data;
    (void)permissions;
    (void)version;

    if (!props)
        return;
    if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0)
        tpw_pw_registry_add_node(reg, id, props);
    else if (strcmp(type, PW_TYPE_INTERFACE_Port) == 0)
        tpw_pw_registry_add_port(reg, id, props);
}

static void tpw_pw_registry_on_global_remove(void* data, uint32_t id)
{
    struct tpw_pw_registry* reg = data;
    bool was_node = false;

    for (size_t i = 0; i < reg->n_nodes; i++) {
        if (reg->nodes[i].id != id)
            continue;
        free(reg->nodes[i].name);
        free(reg->nodes[i].media_class);
        free(reg->nodes[i].description);
        reg->nodes[i] = reg->nodes[--reg->n_nodes];
        was_node = true;
        break;
    }
    for (size_t i = 0; i < reg->n_ports; i++) {
        if (reg->ports[i].id != id)
            continue;
        free(reg->ports[i].name);
        reg->ports[i] = reg->ports[--reg->n_ports];
        break;
    }

    if (was_node && reg->node_removed_cb)
        reg->node_removed_cb(reg->node_removed_data, id);
}

static const struct pw_registry_events tpw_pw_registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = tpw_pw_registry_on_global,
    .global_remove = tpw_pw_registry_on_global_remove,
};

/* Waits for one core round-trip, so every global the server has announced
 * so far has been delivered. The thread loop must already be locked. */
int tpw_pw_core_sync_locked(struct tpw_pw_core_conn* conn)
{
    pw_core_add_listener(conn->core, &conn->core_listener, &tpw_pw_core_events, conn);
    conn->pending_seq = pw_core_sync(conn->core, PW_ID_CORE, 0);
    conn->sync_done = false;
    conn->connect_result = 0;

    struct timespec deadline;
    pw_thread_loop_get_time(conn->loop, &deadline, TPW_CONNECT_TIMEOUT_NSEC);
    while (!conn->sync_done) {
        if (pw_thread_loop_timed_wait_full(conn->loop, &deadline) < 0) {
            conn->connect_result = -ETIMEDOUT;
            break;
        }
    }

    spa_hook_remove(&conn->core_listener);
    return conn->connect_result;
}

int tpw_pw_error_from_sync(int res)
{
    return res == -ETIMEDOUT ? TPW_STREAM_ERR_TIMEOUT : TPW_STREAM_ERR_CONNECT_FAILED;
}

int tpw_pw_registry_bind(struct tpw_pw_registry* reg, struct tpw_pw_core_conn* conn)
{
    if (!reg || !conn || !conn->core)
        return -1;
    if (reg->registry)
        return 0;

    pw_thread_loop_lock(conn->loop);

    reg->registry = pw_core_get_registry(conn->core, PW_VERSION_REGISTRY, 0);
    if (!reg->registry) {
        pw_thread_loop_unlock(conn->loop);
        tpw_log_error("failed to get the pipewire registry");
        return -1;
    }
    pw_registry_add_listener(reg->registry, &reg->listener, &tpw_pw_registry_events, reg);

    /* A sync right after get_registry marks the end of the initial burst of
     * global events, so waiting for it means the caches are populated. */
    int res = tpw_pw_core_sync_locked(conn);

    pw_thread_loop_unlock(conn->loop);

    if (res < 0)
        tpw_log_error("the pipewire registry did not finish enumerating (result=%d)", res);

    return res;
}

int tpw_pw_registry_sync(struct tpw_pw_registry* reg, struct tpw_pw_core_conn* conn)
{
    if (!reg || !reg->registry || !conn || !conn->core)
        return -1;

    pw_thread_loop_lock(conn->loop);
    int res = tpw_pw_core_sync_locked(conn);
    pw_thread_loop_unlock(conn->loop);
    return res;
}

void tpw_pw_registry_teardown(struct tpw_pw_registry* reg)
{
    if (!reg)
        return;

    if (reg->registry) {
        spa_hook_remove(&reg->listener);
        pw_proxy_destroy((struct pw_proxy*)reg->registry);
        reg->registry = NULL;
    }

    for (size_t i = 0; i < reg->n_nodes; i++) {
        free(reg->nodes[i].name);
        free(reg->nodes[i].media_class);
        free(reg->nodes[i].description);
    }
    free(reg->nodes);
    reg->nodes = NULL;
    reg->n_nodes = reg->nodes_capacity = 0;

    for (size_t i = 0; i < reg->n_ports; i++)
        free(reg->ports[i].name);
    free(reg->ports);
    reg->ports = NULL;
    reg->n_ports = reg->ports_capacity = 0;
}

uint32_t tpw_pw_registry_find_node_by_name(const struct tpw_pw_registry* reg, const char* name)
{
    if (!reg || !name)
        return 0;
    for (size_t i = 0; i < reg->n_nodes; i++) {
        if (strcmp(reg->nodes[i].name, name) == 0)
            return reg->nodes[i].id;
    }
    return 0;
}

uint32_t tpw_pw_registry_find_node_by_serial(const struct tpw_pw_registry* reg, uint64_t serial)
{
    if (!reg || serial == 0)
        return 0;
    for (size_t i = 0; i < reg->n_nodes; i++) {
        if (reg->nodes[i].serial == serial)
            return reg->nodes[i].id;
    }
    return 0;
}

uint32_t tpw_pw_registry_find_port(const struct tpw_pw_registry* reg, uint32_t node_id, const char* name,
                                    enum spa_direction direction)
{
    if (!reg || !name)
        return 0;
    for (size_t i = 0; i < reg->n_ports; i++) {
        const struct tpw_pw_port_entry* e = &reg->ports[i];
        if (e->node_id == node_id && e->direction == direction && strcmp(e->name, name) == 0)
            return e->id;
    }
    return 0;
}

size_t tpw_pw_registry_list_ports(const struct tpw_pw_registry* reg, uint32_t node_id,
                                   enum spa_direction direction, const struct tpw_pw_port_entry** out,
                                   size_t max)
{
    if (!reg)
        return 0;

    size_t found = 0;
    for (size_t i = 0; i < reg->n_ports; i++) {
        const struct tpw_pw_port_entry* e = &reg->ports[i];
        if (e->node_id != node_id || e->direction != direction)
            continue;
        found++;
        if (!out || found > max)
            continue;
        /* Insertion sort by ordinal: registry order is arrival order, and a
         * node rarely has more than a handful of ports per direction. */
        size_t at = found - 1;
        while (at > 0 && out[at - 1]->ordinal > e->ordinal) {
            out[at] = out[at - 1];
            at--;
        }
        out[at] = e;
    }
    return found;
}
