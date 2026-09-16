/* SPDX-License-Identifier: MIT */

#include <unistd.h>

#include "tpw/tpw_filter.h"
#include "tpw_test.h"

static int g_cycles = 0;
static size_t g_last_n_buffers = 0;
static int64_t g_last_output_pts = 0;

static void process_cb(tpw_filter_h filter, tpw_filter_port_buffer* buffers, size_t n_buffers, void* user_data)
{
    (void)filter;
    (void)user_data;
    g_cycles++;
    g_last_n_buffers = n_buffers;
    /* Output buffers are left untouched (size stays 0); this must not
     * be treated as an error by the library. Port 2 (added last) is
     * the output port; pts is always -1 there, per tpw_filter.h. */
    if (n_buffers > 2)
        g_last_output_pts = buffers[2].pts;
}

static int g_mixed_cycles = 0;
static size_t g_mixed_n_buffers = 0;
static int64_t g_mixed_event_pts = 0;

static void mixed_process_cb(tpw_filter_h filter, tpw_filter_port_buffer* buffers, size_t n_buffers, void* user_data)
{
    (void)filter;
    (void)user_data;
    g_mixed_cycles++;
    g_mixed_n_buffers = n_buffers;
    /* Port 3 (added last) is the event port; pts is always -1 there,
     * per tpw_filter.h. */
    if (n_buffers > 3)
        g_mixed_event_pts = buffers[3].pts;
}

static int g_dmabuf_mixed_cycles = 0;
static size_t g_dmabuf_mixed_n_buffers = 0;

static void dmabuf_mixed_process_cb(tpw_filter_h filter, tpw_filter_port_buffer* buffers, size_t n_buffers,
                                    void* user_data)
{
    (void)filter;
    (void)buffers;
    (void)user_data;
    g_dmabuf_mixed_cycles++;
    g_dmabuf_mixed_n_buffers = n_buffers;
}

static int g_many_cycles = 0;
static size_t g_many_n_buffers = 0;

static void many_ports_process_cb(tpw_filter_h filter, tpw_filter_port_buffer* buffers, size_t n_buffers,
                                   void* user_data)
{
    (void)filter;
    (void)buffers;
    (void)user_data;
    g_many_cycles++;
    g_many_n_buffers = n_buffers;
}

int main(void)
{
    tpw_filter_h filter = tpw_filter_create("tpw-test-process", process_cb, NULL);
    TPW_ASSERT(filter != NULL);

    tpw_audio_config cfg = { .sample_rate = 48000, .channels = 2 };
    TPW_ASSERT(tpw_filter_add_audio_port(filter, TPW_FILTER_PORT_INPUT, &cfg) != NULL);
    TPW_ASSERT(tpw_filter_add_audio_port(filter, TPW_FILTER_PORT_INPUT, &cfg) != NULL);
    TPW_ASSERT(tpw_filter_add_audio_port(filter, TPW_FILTER_PORT_OUTPUT, &cfg) != NULL);

    TPW_ASSERT_EQ(tpw_filter_start(filter), TPW_OK);
    sleep(1);

    /* All three ports' buffers must arrive together, every cycle. */
    TPW_ASSERT(g_cycles > 0);
    TPW_ASSERT_EQ(g_last_n_buffers, (size_t)3);
    TPW_ASSERT_EQ(g_last_output_pts, (int64_t)-1);

    /* stop() must actually halt delivery, not just stop returning new
     * cycles eventually — no further cycles once stopped. */
    tpw_filter_stop(filter, false);
    int cycles_after_stop = g_cycles;
    sleep(1);
    TPW_ASSERT_EQ(g_cycles, cycles_after_stop);

    tpw_filter_destroy(filter);

    /* One port of each of the four supported kinds on a single filter
     * must still be delivered together in one callback invocation per
     * cycle (audio/video/signal/event mixing). */
    tpw_filter_h mixed = tpw_filter_create("tpw-test-process-mixed", mixed_process_cb, NULL);
    TPW_ASSERT(mixed != NULL);

    TPW_ASSERT(tpw_filter_add_audio_port(mixed, TPW_FILTER_PORT_INPUT, &cfg) != NULL);
    tpw_video_config vcfg = { .width = 640, .height = 480, .pixel_format = "RGB", .fps = 30 };
    TPW_ASSERT(tpw_filter_add_video_port(mixed, TPW_FILTER_PORT_INPUT, &vcfg) != NULL);
    TPW_ASSERT(tpw_filter_add_signal_port(mixed, TPW_FILTER_PORT_INPUT) != NULL);
    TPW_ASSERT(tpw_filter_add_event_port(mixed, TPW_FILTER_PORT_INPUT) != NULL);

    TPW_ASSERT_EQ(tpw_filter_start(mixed), TPW_OK);
    sleep(1);

    TPW_ASSERT(g_mixed_cycles > 0);
    TPW_ASSERT_EQ(g_mixed_n_buffers, (size_t)4);
    TPW_ASSERT_EQ(g_mixed_event_pts, (int64_t)-1);

    tpw_filter_stop(mixed, false);
    tpw_filter_destroy(mixed);

    /* A DMABUF video input port mixed with audio and signal ports must
     * still be delivered together in one callback per cycle, even with no
     * DMABUF source linked — the video port simply carries no frame that
     * cycle. */
    tpw_filter_h dmabuf_mixed = tpw_filter_create("tpw-test-process-dmabuf", dmabuf_mixed_process_cb, NULL);
    TPW_ASSERT(dmabuf_mixed != NULL);

    TPW_ASSERT(tpw_filter_add_audio_port(dmabuf_mixed, TPW_FILTER_PORT_INPUT, &cfg) != NULL);
    tpw_filter_port_opts dmabuf_opts = { .memory = TPW_PORT_MEMORY_DMABUF };
    TPW_ASSERT(tpw_filter_add_video_port_ex(dmabuf_mixed, TPW_FILTER_PORT_INPUT, &vcfg, &dmabuf_opts) != NULL);
    TPW_ASSERT(tpw_filter_add_signal_port(dmabuf_mixed, TPW_FILTER_PORT_INPUT) != NULL);

    TPW_ASSERT_EQ(tpw_filter_start(dmabuf_mixed), TPW_OK);
    sleep(1);

    TPW_ASSERT(g_dmabuf_mixed_cycles > 0);
    TPW_ASSERT_EQ(g_dmabuf_mixed_n_buffers, (size_t)3);

    tpw_filter_stop(dmabuf_mixed, false);
    tpw_filter_destroy(dmabuf_mixed);

    /* More ports than the internal stack-allocation threshold (8) must
     * fall back to heap allocation for the per-cycle buffer array
     * without breaking delivery. */
    tpw_filter_h many = tpw_filter_create("tpw-test-process-many", many_ports_process_cb, NULL);
    TPW_ASSERT(many != NULL);

    const size_t n_ports = 9;
    for (size_t i = 0; i < n_ports; i++)
        TPW_ASSERT(tpw_filter_add_audio_port(many, TPW_FILTER_PORT_INPUT, &cfg) != NULL);

    TPW_ASSERT_EQ(tpw_filter_start(many), TPW_OK);
    sleep(1);

    TPW_ASSERT(g_many_cycles > 0);
    TPW_ASSERT_EQ(g_many_n_buffers, n_ports);

    tpw_filter_stop(many, false);
    tpw_filter_destroy(many);
    return 0;
}
