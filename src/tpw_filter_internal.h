/* SPDX-License-Identifier: MIT */

#ifndef TPW_FILTER_INTERNAL_H
#define TPW_FILTER_INTERNAL_H

#include <pthread.h>
#include <stdbool.h>

#include <pipewire/filter.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/video/format-utils.h>

#include "tpw/tpw_filter.h"
#include "tpw_pw_core_internal.h"

enum tpw_filter_state {
    TPW_FILTER_STATE_CREATED,
    TPW_FILTER_STATE_RUNNING,
    TPW_FILTER_STATE_STOPPED,
};

/* Defined in tpw_filter_port_link.c; only ever held as a pointer here. */
struct tpw_link_wait;
struct pw_link_info;

struct tpw_filter_audio_port_state {
    int sample_rate;
    int channels;
    enum spa_audio_format format;
};

struct tpw_filter_video_port_state {
    int width;
    int height;
    enum spa_video_format format;
};

/* One event staged via tpw_filter_port_push_event(), owning a copy of
 * the caller's data (since it's only guaranteed valid for that call,
 * not for the rest of the cycle). `key`, when set, points at a static
 * name from tpw_filter_event.c's property vocabulary table rather
 * than an owned copy, since that table outlives every filter. */
struct tpw_filter_pending_event {
    uint32_t offset;
    tpw_event_kind kind;
    const char* key;  /* static vocabulary entry, NULL unless kind == TPW_EVENT_PROPERTY */
    void* data;       /* owned copy */
    size_t size;
};

/* One input or output port on a filter. This struct IS the PipeWire
 * port's user-data block (pw_filter_add_port() allocates it inline), so
 * a tpw_filter_port_h is just this pointer. Only resolved values are
 * kept (not the caller's tpw_video_config, whose pixel_format pointer
 * isn't guaranteed to outlive the call that added the port). */
struct tpw_filter_port {
    struct tpw_filter* filter;
    tpw_filter_port_direction direction;
    tpw_stream_type media_type;
    union {
        struct tpw_filter_audio_port_state audio;
        struct tpw_filter_video_port_state video;
    } config;

    /* Staged application-pushed buffer (input ports only), consumed and
     * cleared on the next processing cycle. pushed_pts is the pts
     * argument tpw_filter_push_port_data() was called with, carried
     * through to that cycle's tpw_filter_port_buffer.pts. */
    void* pushed_data;
    size_t pushed_size;
    size_t pushed_capacity;
    int64_t pushed_pts;
    bool pushed_pending; /* A push awaits delivery, which happens exactly once. */

    /* This is the data thread's side of a push swap, read by the callback and by
     * hold. Only that thread touches it, so a later push never changes it. */
    void* delivered_data;
    size_t delivered_capacity;

    /* Event ports only. incoming_events is this cycle's delivered
     * events (input direction), read via tpw_filter_port_get_event();
     * its data/key pointers alias either the dequeued control
     * sequence's memory or a delivering_events entry's owned copy
     * — both stay valid for the rest of the cycle, so incoming_events
     * itself never owns the bytes it points to.
     *
     * pending_events serves two different purposes depending on
     * `direction`, since a given port is only ever one direction:
     *   - INPUT: events staged via tpw_filter_port_push_event(),
     *     consumed into incoming_events (in place of a real dequeue)
     *     and freed at the end of the cycle that delivers them —
     *     mirrors pushed_data's "next cycle" staging above.
     *   - OUTPUT: events staged via tpw_filter_port_push_event() during
     *     the current cycle's callback, encoded into a control sequence
     *     and freed once the callback returns.
     * event_output_capacity (output direction only) is the current
     * cycle's dequeued buffer's maximum byte size, set before the
     * callback runs so tpw_filter_port_push_event() can reject a push
     * that wouldn't fit instead of silently truncating later. */
    tpw_event* incoming_events;
    size_t n_incoming_events;
    size_t incoming_events_capacity;

    struct tpw_filter_pending_event* pending_events;
    size_t n_pending_events;
    size_t pending_events_capacity;
    size_t event_output_capacity;

    /* On input ports these are the events swapped out of pending_events for this
     * cycle, and the data thread owns them until the cycle frees them. */
    struct tpw_filter_pending_event* delivering_events;
    size_t n_delivering_events;
    size_t delivering_events_capacity;

    /* DMABUF import (video input ports added via _ex with DMABUF). When
     * set, the port negotiates DmaBuf buffers (no MAP_BUFFERS) and
     * current_dmabuf_buf points at this cycle's dequeued buffer for the
     * accessor to read; NULL outside a cycle or when no buffer arrived. */
    bool use_dmabuf;
    struct spa_buffer* current_dmabuf_buf;

    /* Hold: when enabled, an input cycle with no new data re-presents the
     * port's most recent buffer. `held` is the retained dequeued PipeWire
     * buffer (NULL when the last buffer came from push), returned to the
     * source only when a new buffer arrives. has_held/held_* snapshot the
     * last delivered buffer so it can be re-presented; held_dmabuf_buf is
     * what the accessor reads on a held cycle. update_seq backs
     * tpw_filter_port_buffer.seq and advances only on genuinely new data. */
    bool hold_enabled;
    bool has_held;
    struct pw_buffer* held;
    struct spa_buffer* held_dmabuf_buf;
    void* held_data;
    size_t held_size;
    int64_t held_pts;
    uint64_t update_seq;

    /* Core link (tpw_filter_port_link). pw_port_name is the PW_KEY_PORT_NAME
     * this port was created with, used to find own_global_id in the registry.
     * link_proxy owns the created link; link_state_seen_active distinguishes
     * "still negotiating" from "was up, then the source vanished". */
    char pw_port_name[24];
    uint32_t own_global_id;
    struct pw_proxy* link_proxy;
    struct spa_hook link_listener;
    bool link_state_seen_active;
    bool link_lost;                 /* the link died after being up; proxy still
                                       needs destroying by its owner */
    bool loss_reported;             /* It is set once a lost source is reported or the application
                                       released the link itself, and a new format clears it. */
    struct tpw_link_wait* link_wait; /* set only while a link call is blocked */
};

/* One multi-port filter. */
struct tpw_filter {
    char* name;
    enum tpw_filter_state state;

    struct tpw_filter_port** ports;
    size_t n_ports;
    size_t ports_capacity;

    tpw_filter_process_cb process_cb;
    tpw_filter_error_cb error_cb;
    void* user_data;

    struct tpw_pw_core_conn conn;
    struct pw_filter* pw_filter;
    struct spa_hook filter_listener;

    /* This lock guards what input-port pushes stage. The cycle only try-locks
     * it, so the data thread never waits on an application thread. */
    pthread_mutex_t push_lock;

    /* Set false, then true by .drained, around a draining
     * tpw_filter_stop()'s pw_filter_flush() call. */
    bool drained;

    /* Optional preferred maximum bundling period (nanoseconds); 0 = unset.
     * Applied as a node.latency preference before the first connect. */
    uint32_t period_hint_ns;

    /* Registry view of the graph, bound lazily on the first port link and
     * used to resolve target/own port ids. */
    struct tpw_pw_registry registry;
};

/* Appends `port` to filter->ports, growing the array as needed. Returns
 * false on allocation failure. */
bool tpw_filter_add_port_to_list(struct tpw_filter* filter, struct tpw_filter_port* port);

/* .process callback registered on the underlying pw_filter; assembles
 * one tpw_filter_port_buffer per port (consuming any staged pushed
 * buffer first) and invokes the developer's process_cb once. */
void tpw_filter_on_process(void* data, struct spa_io_position* position);

/* The filter whose process callback this thread is currently running, or
 * NULL. Taking the thread-loop lock from inside that callback deadlocks
 * against PipeWire's own buffer setup, so the push helpers skip it. */
extern _Thread_local const struct tpw_filter* tpw_filter_processing;

/* Claims the single report of `port` losing its source while the filter runs.
 * Returns false when it was already reported, or the application released the link itself. */
bool tpw_filter_claim_source_loss(struct tpw_filter* filter, struct tpw_filter_port* port);

/* .param_changed callback registered on the underlying pw_filter;
 * treats a port's format being cleared (param == NULL for
 * SPA_PARAM_Format) as that port's source becoming unavailable. */
void tpw_filter_on_param_changed(void* data, void* port_data, uint32_t id, const struct spa_pod* param);

/* Decodes a dequeued control sequence buffer (`data`/`size`) into
 * `port`'s incoming_events for the current cycle. Safe to call with
 * data == NULL or a buffer that isn't a valid control sequence — both
 * simply leave incoming_events empty for that cycle. */
void tpw_filter_event_decode(struct tpw_filter_port* port, const void* data, size_t size);

/* Swaps an input event port's pending_events into delivering_events, which
 * must be empty. The caller holds the filter's push_lock. */
void tpw_filter_event_take_pending(struct tpw_filter_port* port);

/* Points incoming_events at delivering_events' owned memory for this cycle,
 * in place of a real dequeue. */
void tpw_filter_event_load_delivering_as_incoming(struct tpw_filter_port* port);

/* Frees delivering_events' owned data once the cycle that delivered it ends. */
void tpw_filter_event_clear_delivering(struct tpw_filter_port* port);

/* Frees the owned data of every entry in `port`'s pending_events and
 * resets the list to empty. Safe to call when there is nothing staged. */
void tpw_filter_event_clear_pending(struct tpw_filter_port* port);

/* Encodes `port`'s pending_events (staged via tpw_filter_port_push_event
 * on an output event port during the just-finished cycle) into
 * `buf`/`maxsize` as a control sequence, then clears pending_events.
 * Returns the number of bytes written (0 if there was nothing to
 * encode). Assumes tpw_filter_port_push_event() already rejected any
 * push that wouldn't fit in `maxsize`. */
size_t tpw_filter_event_finish_output(struct tpw_filter_port* port, void* buf, size_t maxsize);

/* Frees all owned event memory for `port` (pending_events and the
 * incoming_events array itself); a no-op for a non-event port. Called
 * from tpw_filter_destroy(). */
void tpw_filter_event_free_port(struct tpw_filter_port* port);

/* Emits the DmaBuf SPA_PARAM_Buffers param for a negotiated `use_dmabuf`
 * port via pw_filter_update_params(); a no-op for a non-DMABUF port.
 * Called from param_changed once the port's format is set. */
void tpw_filter_dmabuf_update_params(struct tpw_filter_port* port);

/* Logs (WARNING) that a DMABUF port's source could not provide DMABUF, so
 * the port delivers no buffers. A no-op for a non-DMABUF port. */
void tpw_filter_dmabuf_log_unavailable(struct tpw_filter_port* port);

/* Numerator of the "num/48000" node.latency time ratio for a period hint
 * of `period_ns` nanoseconds (floored, min 1). Exposed for unit testing. */
uint32_t tpw_filter_period_hint_num(uint32_t period_ns);

/* .info callback registered on a port's link proxy; reports a link that
 * was up and has since gone away as the source becoming unavailable.
 * Exposed (rather than static) so a unit test can drive it directly. */
void tpw_filter_link_on_info(void* data, const struct pw_link_info* info);

/* Releases any link held by any of `filter`'s ports. Called from
 * tpw_filter_stop()/tpw_filter_destroy(); safe when nothing is linked. */
void tpw_filter_release_all_links(struct tpw_filter* filter);

#endif /* TPW_FILTER_INTERNAL_H */
