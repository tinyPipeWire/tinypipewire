/* SPDX-License-Identifier: MIT */

#include <stdlib.h>

#include <spa/buffer/buffer.h>
#include <spa/buffer/meta.h>

#include "tpw_dmabuf_internal.h"
#include "tpw_filter_internal.h"

#define TPW_FILTER_STACK_PORTS 8

_Thread_local const struct tpw_filter* tpw_filter_processing;

/* SPA_META_Header carries the source's capture clock (ALSA/V4L2, etc.);
 * not every dequeued buffer has one, so -1 signals "unavailable" rather
 * than guessing a timestamp. */
static int64_t tpw_filter_buffer_pts(struct pw_buffer* b)
{
    struct spa_meta_header* h = spa_buffer_find_meta_data(b->buffer, SPA_META_Header, sizeof(*h));
    return h ? h->pts : -1;
}

/* Whether a dequeued input buffer actually carries data this cycle. A
 * DMABUF port is not CPU-mapped, so its data pointer is always NULL —
 * presence is the DmaBuf data-type instead. */
static bool tpw_filter_input_buffer_present(struct tpw_filter_port* port, struct pw_buffer* b)
{
    if (!b || b->buffer->n_datas == 0)
        return false;
    if (port->use_dmabuf)
        return tpw_dmabuf_buffer_present(b->buffer);
    return b->buffer->datas[0].data != NULL;
}

void tpw_filter_on_process(void* data, struct spa_io_position* position)
{
    struct tpw_filter* filter = data;
    (void)position;

    if (filter->n_ports == 0)
        return;

    tpw_filter_port_buffer stack_buffers[TPW_FILTER_STACK_PORTS];
    struct pw_buffer* stack_dequeued[TPW_FILTER_STACK_PORTS];
    bool heap_alloc = filter->n_ports > TPW_FILTER_STACK_PORTS;

    tpw_filter_port_buffer* buffers = stack_buffers;
    struct pw_buffer** dequeued = stack_dequeued;
    if (heap_alloc) {
        buffers = calloc(filter->n_ports, sizeof(*buffers));
        dequeued = calloc(filter->n_ports, sizeof(*dequeued));
        if (!buffers || !dequeued) {
            free(buffers);
            free(dequeued);
            return;
        }
    }

    /* Pushes are staged under push_lock and swapped in here. If a push holds
     * the lock right now, staged data waits for the next cycle rather than us. */
    bool staged = pthread_mutex_trylock(&filter->push_lock) == 0;

    for (size_t i = 0; i < filter->n_ports; i++) {
        struct tpw_filter_port* port = filter->ports[i];
        buffers[i].port = (tpw_filter_port_h)port;
        buffers[i].data = NULL;
        buffers[i].size = 0;
        buffers[i].capacity = 0;
        buffers[i].pts = -1;
        buffers[i].fresh = false;
        buffers[i].seq = 0;
        dequeued[i] = NULL;
        port->current_dmabuf_buf = NULL;

        if (port->media_type == TPW_STREAM_TYPE_EVENT) {
            if (port->direction == TPW_FILTER_PORT_INPUT) {
                if (staged && port->n_pending_events > 0)
                    tpw_filter_event_take_pending(port);
                if (port->n_delivering_events > 0) {
                    /* Application-pushed events take priority over the
                     * graph this cycle, same as pushed data below. */
                    tpw_filter_event_load_delivering_as_incoming(port);
                } else {
                    struct pw_buffer* b = pw_filter_dequeue_buffer(port);
                    if (b && b->buffer->datas[0].data) {
                        struct spa_data* d = &b->buffer->datas[0];
                        tpw_filter_event_decode(port, d->data, d->chunk ? d->chunk->size : 0);
                        dequeued[i] = b;
                    } else {
                        port->n_incoming_events = 0;
                    }
                }
            } else {
                port->n_pending_events = 0;
                struct pw_buffer* b = pw_filter_dequeue_buffer(port);
                if (b && b->buffer->datas[0].data) {
                    port->event_output_capacity = b->buffer->datas[0].maxsize;
                    dequeued[i] = b;
                } else {
                    port->event_output_capacity = 0;
                }
            }
            continue;
        }

        if (port->direction == TPW_FILTER_PORT_INPUT) {
            bool got_new = false;

            if (staged && port->pushed_pending) {
                /* Application-pushed data takes priority over the graph. The swap
                 * leaves the push side a buffer this thread no longer reads. */
                void* data = port->pushed_data;
                size_t capacity = port->pushed_capacity;
                port->pushed_data = port->delivered_data;
                port->pushed_capacity = port->delivered_capacity;
                port->delivered_data = data;
                port->delivered_capacity = capacity;

                buffers[i].data = data;
                buffers[i].size = port->pushed_size;
                buffers[i].pts = port->pushed_pts;
                port->pushed_pending = false;
                got_new = true;
            } else {
                struct pw_buffer* b = pw_filter_dequeue_buffer(port);
                if (b) {
                    dequeued[i] = b;
                    if (tpw_filter_input_buffer_present(port, b)) {
                        struct spa_data* d = &b->buffer->datas[0];
                        if (port->use_dmabuf) {
                            /* Not CPU-mapped: reach the frame via the accessor. */
                            port->current_dmabuf_buf = b->buffer;
                        } else {
                            buffers[i].data = d->data;
                            buffers[i].size = d->chunk ? d->chunk->size : 0;
                        }
                        buffers[i].pts = tpw_filter_buffer_pts(b);
                        got_new = true;
                    }
                }
            }

            if (got_new) {
                buffers[i].fresh = true;
                buffers[i].seq = ++port->update_seq;
                if (port->hold_enabled) {
                    /* Retain this buffer for re-presentation; release the
                     * previously held one now that it has a replacement. */
                    if (port->held)
                        pw_filter_queue_buffer(port, port->held);
                    port->held = dequeued[i];
                    dequeued[i] = NULL; /* held: not requeued at cycle end */
                    port->has_held = true;
                    port->held_data = buffers[i].data;
                    port->held_size = buffers[i].size;
                    port->held_pts = buffers[i].pts;
                    port->held_dmabuf_buf = port->current_dmabuf_buf;
                }
            } else if (port->hold_enabled && port->has_held) {
                /* No new data this cycle: re-present the retained buffer. */
                buffers[i].data = port->held_data;
                buffers[i].size = port->held_size;
                buffers[i].pts = port->held_pts;
                port->current_dmabuf_buf = port->held_dmabuf_buf;
                buffers[i].seq = port->update_seq;
            } else {
                buffers[i].seq = port->update_seq;
            }
        } else {
            struct pw_buffer* b = pw_filter_dequeue_buffer(port);
            if (b && b->buffer->datas[0].data) {
                struct spa_data* d = &b->buffer->datas[0];
                buffers[i].data = d->data;
                buffers[i].capacity = d->maxsize;
                dequeued[i] = b;
            }
        }
    }

    if (staged)
        pthread_mutex_unlock(&filter->push_lock);

    if (filter->process_cb) {
        /* Marked for the duration of the callback so the push helpers know
         * not to take this filter's loop lock from inside it. */
        const struct tpw_filter* outer = tpw_filter_processing;
        tpw_filter_processing = filter;
        filter->process_cb((tpw_filter_h)filter, buffers, filter->n_ports, filter->user_data);
        tpw_filter_processing = outer;
    }

    for (size_t i = 0; i < filter->n_ports; i++) {
        struct tpw_filter_port* port = filter->ports[i];

        if (port->media_type == TPW_STREAM_TYPE_EVENT) {
            if (port->direction == TPW_FILTER_PORT_INPUT) {
                tpw_filter_event_clear_delivering(port);
            } else if (dequeued[i]) {
                struct spa_data* d = &dequeued[i]->buffer->datas[0];
                size_t encoded = tpw_filter_event_finish_output(port, d->data, d->maxsize);
                if (d->chunk) {
                    d->chunk->size = (uint32_t)encoded;
                    d->chunk->offset = 0;
                    d->chunk->stride = 0;
                }
            } else {
                tpw_filter_event_clear_pending(port);
            }
            if (dequeued[i])
                pw_filter_queue_buffer(port, dequeued[i]);
            continue;
        }

        if (!dequeued[i])
            continue;

        if (port->direction == TPW_FILTER_PORT_OUTPUT) {
            struct spa_data* d = &dequeued[i]->buffer->datas[0];
            if (d->chunk) {
                d->chunk->size = (uint32_t)buffers[i].size;
                d->chunk->offset = 0;
                d->chunk->stride = 0;
            }
        }
        pw_filter_queue_buffer(port, dequeued[i]);
    }

    if (heap_alloc) {
        free(buffers);
        free(dequeued);
    }
}
