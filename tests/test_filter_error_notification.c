/* SPDX-License-Identifier: MIT */

/* Whitebox test: simulates a port's format being cleared (PipeWire's
 * signal that whatever fed it is gone) by calling the param_changed
 * handler directly, rather than physically removing a device. */

#include "tpw_filter_internal.h"
#include "tpw_test.h"

static int g_error_calls = 0;
static tpw_filter_port_h g_last_port = NULL;
static int g_last_error_code = 0;

static void noop_process_cb(tpw_filter_h filter, tpw_filter_port_buffer* buffers, size_t n_buffers,
                             void* user_data)
{
    (void)filter;
    (void)buffers;
    (void)n_buffers;
    (void)user_data;
}

static void on_error(tpw_filter_h filter, tpw_filter_port_h port, int error_code, void* user_data)
{
    (void)filter;
    (void)user_data;
    g_error_calls++;
    g_last_port = port;
    g_last_error_code = error_code;
}

int main(void)
{
    tpw_filter_h handle = tpw_filter_create("tpw-test-error", noop_process_cb, NULL);
    TPW_ASSERT(handle != NULL);
    TPW_ASSERT_EQ(tpw_filter_set_error_cb(handle, on_error), TPW_OK);

    tpw_audio_config cfg = { .sample_rate = 48000, .channels = 2 };
    tpw_filter_port_h port_a = tpw_filter_add_audio_port(handle, TPW_FILTER_PORT_INPUT, &cfg);
    tpw_filter_port_h port_b = tpw_filter_add_audio_port(handle, TPW_FILTER_PORT_INPUT, &cfg);
    TPW_ASSERT(port_a != NULL);
    TPW_ASSERT(port_b != NULL);
    TPW_ASSERT_EQ(tpw_filter_start(handle), TPW_OK);

    struct tpw_filter* filter = (struct tpw_filter*)handle;

    /* port_a loses its format (source gone); port_b is unaffected. */
    tpw_filter_on_param_changed(filter, (struct tpw_filter_port*)port_a, SPA_PARAM_Format, NULL);

    TPW_ASSERT_EQ(g_error_calls, 1);
    TPW_ASSERT(g_last_port == port_a);
    TPW_ASSERT_EQ(g_last_error_code, TPW_ERR_SOURCE_UNAVAILABLE);

    /* An unrelated param id, or a non-NULL format, must not fire the callback. */
    tpw_filter_on_param_changed(filter, (struct tpw_filter_port*)port_b, SPA_PARAM_Props, NULL);
    TPW_ASSERT_EQ(g_error_calls, 1);

    tpw_filter_stop(handle, false);
    tpw_filter_destroy(handle);
    return 0;
}
