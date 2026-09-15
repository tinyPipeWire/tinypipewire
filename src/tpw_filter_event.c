/* SPDX-License-Identifier: MIT */

#include <stdlib.h>
#include <string.h>

#include <spa/control/control.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>

#include "tpw_filter_internal.h"

struct tpw_property_entry {
    const char* name;
    uint32_t spa_prop_id;
};

/* A starter vocabulary of well-known SPA properties, translated to/from
 * their real enum spa_prop id so property events stay wire-compatible
 * with other PipeWire clients. Grows without changing the public API
 * shape (tpw_event.key is always a plain string). */
static const struct tpw_property_entry tpw_property_table[] = {
    { "volume", SPA_PROP_volume },
    { "mute", SPA_PROP_mute },
    { "channelVolumes", SPA_PROP_channelVolumes },
};

static const struct tpw_property_entry* tpw_property_find_by_name(const char* name)
{
    if (!name)
        return NULL;
    for (size_t i = 0; i < SPA_N_ELEMENTS(tpw_property_table); i++) {
        if (strcmp(tpw_property_table[i].name, name) == 0)
            return &tpw_property_table[i];
    }
    return NULL;
}

static const struct tpw_property_entry* tpw_property_find_by_id(uint32_t id)
{
    for (size_t i = 0; i < SPA_N_ELEMENTS(tpw_property_table); i++) {
        if (tpw_property_table[i].spa_prop_id == id)
            return &tpw_property_table[i];
    }
    return NULL;
}

static bool tpw_event_kind_key_valid(tpw_event_kind kind, const char* key)
{
    if (kind == TPW_EVENT_PROPERTY)
        return tpw_property_find_by_name(key) != NULL;
    if (kind == TPW_EVENT_MIDI || kind == TPW_EVENT_OSC)
        return key == NULL;
    return false; /* TPW_EVENT_UNKNOWN (or any other value) can't be pushed */
}

static bool tpw_filter_incoming_event_append(struct tpw_filter_port* port, const tpw_event* event)
{
    if (port->n_incoming_events == port->incoming_events_capacity) {
        size_t new_cap = port->incoming_events_capacity == 0 ? 4 : port->incoming_events_capacity * 2;
        tpw_event* grown = realloc(port->incoming_events, new_cap * sizeof(*grown));
        if (!grown)
            return false;
        port->incoming_events = grown;
        port->incoming_events_capacity = new_cap;
    }
    port->incoming_events[port->n_incoming_events++] = *event;
    return true;
}

void tpw_filter_event_decode(struct tpw_filter_port* port, const void* data, size_t size)
{
    port->n_incoming_events = 0;
    if (!data || size < sizeof(struct spa_pod))
        return;

    struct spa_pod* pod = spa_pod_from_data((void*)data, size, 0, size);
    if (!pod || !spa_pod_is_sequence(pod))
        return;

    struct spa_pod_sequence* seq = (struct spa_pod_sequence*)pod;
    struct spa_pod_control* c;
    SPA_POD_SEQUENCE_FOREACH(seq, c) {
        const struct spa_pod* value = &c->value;

        if (c->type == SPA_CONTROL_Midi || c->type == SPA_CONTROL_OSC) {
            const void* bytes = NULL;
            uint32_t len = 0;
            if (spa_pod_get_bytes(value, &bytes, &len) < 0)
                continue;
            tpw_event ev = {
                .offset = c->offset,
                .kind = (c->type == SPA_CONTROL_Midi) ? TPW_EVENT_MIDI : TPW_EVENT_OSC,
                .key = NULL,
                .data = bytes,
                .size = len,
            };
            tpw_filter_incoming_event_append(port, &ev);
        } else if (c->type == SPA_CONTROL_Properties) {
            if (!spa_pod_is_object_type(value, SPA_TYPE_OBJECT_Props))
                continue;
            const struct spa_pod_object* obj = (const struct spa_pod_object*)value;
            struct spa_pod_prop* p;
            SPA_POD_OBJECT_FOREACH(obj, p) {
                const struct tpw_property_entry* entry = tpw_property_find_by_id(p->key);
                if (!entry)
                    continue; /* property key outside our vocabulary: skipped */
                tpw_event ev = {
                    .offset = c->offset,
                    .kind = TPW_EVENT_PROPERTY,
                    .key = entry->name,
                    .data = SPA_POD_BODY(&p->value),
                    .size = SPA_POD_BODY_SIZE(&p->value),
                };
                tpw_filter_incoming_event_append(port, &ev);
            }
        } else {
            tpw_event ev = {
                .offset = c->offset,
                .kind = TPW_EVENT_UNKNOWN,
                .key = NULL,
                .data = SPA_POD_BODY(value),
                .size = SPA_POD_BODY_SIZE(value),
            };
            tpw_filter_incoming_event_append(port, &ev);
        }
    }
}

void tpw_filter_event_take_pending(struct tpw_filter_port* port)
{
    struct tpw_filter_pending_event* events = port->delivering_events;
    size_t capacity = port->delivering_events_capacity;

    port->delivering_events = port->pending_events;
    port->n_delivering_events = port->n_pending_events;
    port->delivering_events_capacity = port->pending_events_capacity;

    port->pending_events = events;
    port->n_pending_events = 0;
    port->pending_events_capacity = capacity;
}

void tpw_filter_event_load_delivering_as_incoming(struct tpw_filter_port* port)
{
    port->n_incoming_events = 0;
    for (size_t i = 0; i < port->n_delivering_events; i++) {
        struct tpw_filter_pending_event* pe = &port->delivering_events[i];
        tpw_event ev = { pe->offset, pe->kind, pe->key, pe->data, pe->size };
        tpw_filter_incoming_event_append(port, &ev);
    }
}

static void tpw_filter_event_free_entries(struct tpw_filter_pending_event* events, size_t* n_events)
{
    for (size_t i = 0; i < *n_events; i++)
        free(events[i].data);
    *n_events = 0;
}

void tpw_filter_event_clear_pending(struct tpw_filter_port* port)
{
    tpw_filter_event_free_entries(port->pending_events, &port->n_pending_events);
}

void tpw_filter_event_clear_delivering(struct tpw_filter_port* port)
{
    tpw_filter_event_free_entries(port->delivering_events, &port->n_delivering_events);
}

static bool tpw_filter_pending_event_append(struct tpw_filter_port* port, const tpw_event* event)
{
    if (port->n_pending_events == port->pending_events_capacity) {
        size_t new_cap = port->pending_events_capacity == 0 ? 4 : port->pending_events_capacity * 2;
        struct tpw_filter_pending_event* grown = realloc(port->pending_events, new_cap * sizeof(*grown));
        if (!grown)
            return false;
        port->pending_events = grown;
        port->pending_events_capacity = new_cap;
    }

    struct tpw_filter_pending_event* dst = &port->pending_events[port->n_pending_events];
    dst->offset = event->offset;
    dst->kind = event->kind;
    dst->key = (event->kind == TPW_EVENT_PROPERTY) ? tpw_property_find_by_name(event->key)->name : NULL;
    dst->data = NULL;
    dst->size = event->size;
    if (event->size > 0) {
        dst->data = malloc(event->size);
        if (!dst->data)
            return false;
        memcpy(dst->data, event->data, event->size);
    }

    port->n_pending_events++;
    return true;
}

static void tpw_filter_pending_event_pop_last(struct tpw_filter_port* port)
{
    if (port->n_pending_events == 0)
        return;
    port->n_pending_events--;
    free(port->pending_events[port->n_pending_events].data);
}

/* Encodes pending_events as-is into buf/maxsize; buf == NULL / maxsize
 * == 0 is a valid "dry run" that reports the size that would be needed
 * without writing anything (the standard SPA POD sizing idiom). */
static size_t tpw_filter_event_encode(struct tpw_filter_port* port, void* buf, size_t maxsize)
{
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, (uint32_t)maxsize);
    struct spa_pod_frame f;
    spa_pod_builder_push_sequence(&b, &f, 0);

    for (size_t i = 0; i < port->n_pending_events; i++) {
        struct tpw_filter_pending_event* pe = &port->pending_events[i];

        if (pe->kind == TPW_EVENT_MIDI || pe->kind == TPW_EVENT_OSC) {
            spa_pod_builder_control(&b, pe->offset, (pe->kind == TPW_EVENT_MIDI) ? SPA_CONTROL_Midi : SPA_CONTROL_OSC);
            spa_pod_builder_bytes(&b, pe->data, (uint32_t)pe->size);
        } else if (pe->kind == TPW_EVENT_PROPERTY) {
            const struct tpw_property_entry* entry = tpw_property_find_by_name(pe->key);
            if (!entry)
                continue; /* validated at push time; defensive only */
            spa_pod_builder_control(&b, pe->offset, SPA_CONTROL_Properties);
            struct spa_pod_frame obj_f;
            spa_pod_builder_push_object(&b, &obj_f, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
            spa_pod_builder_prop(&b, entry->spa_prop_id, 0);
            spa_pod_builder_bytes(&b, pe->data, (uint32_t)pe->size);
            spa_pod_builder_pop(&b, &obj_f);
        }
    }

    spa_pod_builder_pop(&b, &f);
    return b.state.offset;
}

size_t tpw_filter_event_finish_output(struct tpw_filter_port* port, void* buf, size_t maxsize)
{
    size_t encoded = tpw_filter_event_encode(port, buf, maxsize);
    tpw_filter_event_clear_pending(port);
    return encoded;
}

void tpw_filter_event_free_port(struct tpw_filter_port* port)
{
    if (!port || port->media_type != TPW_STREAM_TYPE_EVENT)
        return;
    tpw_filter_event_clear_pending(port);
    tpw_filter_event_clear_delivering(port);
    free(port->pending_events);
    free(port->delivering_events);
    free(port->incoming_events);
}

size_t tpw_filter_port_get_event_count(tpw_filter_port_h port_handle)
{
    struct tpw_filter_port* port = (struct tpw_filter_port*)port_handle;
    if (!port || port->media_type != TPW_STREAM_TYPE_EVENT || port->direction != TPW_FILTER_PORT_INPUT)
        return 0;
    return port->n_incoming_events;
}

int tpw_filter_port_get_event(tpw_filter_port_h port_handle, size_t index, tpw_event* out)
{
    struct tpw_filter_port* port = (struct tpw_filter_port*)port_handle;
    if (!port || port->media_type != TPW_STREAM_TYPE_EVENT || port->direction != TPW_FILTER_PORT_INPUT || !out)
        return TPW_STREAM_ERR_INVALID_ARG;
    if (index >= port->n_incoming_events)
        return TPW_STREAM_ERR_INVALID_ARG;

    *out = port->incoming_events[index];
    return TPW_STREAM_OK;
}

int tpw_filter_port_push_event(tpw_filter_port_h port_handle, const tpw_event* event)
{
    struct tpw_filter_port* port = (struct tpw_filter_port*)port_handle;
    if (!port || port->media_type != TPW_STREAM_TYPE_EVENT || !event)
        return TPW_STREAM_ERR_INVALID_ARG;
    if (event->size > 0 && !event->data)
        return TPW_STREAM_ERR_INVALID_ARG;
    if (!tpw_event_kind_key_valid(event->kind, event->key))
        return TPW_STREAM_ERR_INVALID_ARG;

    struct tpw_filter* filter = port->filter;
    if (port->direction == TPW_FILTER_PORT_INPUT) {
        /* The cycle swaps what is staged here, so the push lock is enough. */
        pthread_mutex_lock(&filter->push_lock);
        bool staged = tpw_filter_pending_event_append(port, event);
        pthread_mutex_unlock(&filter->push_lock);
        return staged ? TPW_STREAM_OK : TPW_STREAM_ERR_NO_MEMORY;
    }

    /* An output port is pushed from inside the processing callback, which
     * already runs on the loop's own thread; locking there would deadlock. */
    bool lock = tpw_filter_processing != filter;
    if (lock)
        pw_thread_loop_lock(filter->conn.loop);

    if (!tpw_filter_pending_event_append(port, event)) {
        if (lock)
            pw_thread_loop_unlock(filter->conn.loop);
        return TPW_STREAM_ERR_NO_MEMORY;
    }

    /* A push must fit the output buffer this cycle dequeued, so an oversized
     * one is rejected here rather than silently truncated at encode time. */
    size_t needed = tpw_filter_event_encode(port, NULL, 0);
    if (needed > port->event_output_capacity) {
        tpw_filter_pending_event_pop_last(port);
        if (lock)
            pw_thread_loop_unlock(filter->conn.loop);
        return TPW_STREAM_ERR_INVALID_ARG;
    }

    if (lock)
        pw_thread_loop_unlock(filter->conn.loop);
    return TPW_STREAM_OK;
}
