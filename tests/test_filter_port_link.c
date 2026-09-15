/* SPDX-License-Identifier: MIT */

/* Link and unlink rejections, plus the source-unavailable notification.
 * Every link here names a node that deliberately does not exist, so no
 * hardware is needed; successful links live in the hardware suite. */

#include <pipewire/link.h>

#include "tpw_filter_internal.h"
#include "tpw_test.h"

/* A node name no real PipeWire graph will have. */
#define ABSENT_NODE "tpw-no-such-node-12345"

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

static const tpw_audio_config g_audio_cfg = { .sample_rate = 48000, .channels = 2 };

/* Argument validation that needs no started filter. */
static void test_invalid_args(void)
{
    tpw_filter_h handle = tpw_filter_create("tpw-test-link-args", noop_process_cb, NULL);
    TPW_ASSERT(handle != NULL);

    tpw_filter_port_h in = tpw_filter_add_audio_port(handle, TPW_FILTER_PORT_INPUT, &g_audio_cfg);
    tpw_filter_port_h out = tpw_filter_add_audio_port(handle, TPW_FILTER_PORT_OUTPUT, &g_audio_cfg);
    TPW_ASSERT(in != NULL && out != NULL);

    /* NULL handles and empty targets are rejected before anything else. */
    TPW_ASSERT_EQ(tpw_filter_port_link(NULL, ABSENT_NODE), TPW_STREAM_ERR_INVALID_ARG);
    TPW_ASSERT_EQ(tpw_filter_port_link(in, NULL), TPW_STREAM_ERR_INVALID_ARG);
    TPW_ASSERT_EQ(tpw_filter_port_link(in, ""), TPW_STREAM_ERR_INVALID_ARG);
    TPW_ASSERT_EQ(tpw_filter_port_unlink(NULL), TPW_STREAM_ERR_INVALID_ARG);

    /* Output ports are out of scope for this feature, in both directions
     * of the API, and are rejected before the filter's state matters. */
    TPW_ASSERT_EQ(tpw_filter_port_link(out, ABSENT_NODE), TPW_STREAM_ERR_INVALID_ARG);
    TPW_ASSERT_EQ(tpw_filter_port_unlink(out), TPW_STREAM_ERR_INVALID_ARG);

    /* Linking needs a live graph, so it is the one port call that must come
     * after start() rather than before it. */
    TPW_ASSERT_EQ(tpw_filter_port_link(in, ABSENT_NODE), TPW_STREAM_ERR_NOT_CONFIGURED);

    tpw_filter_destroy(handle);
}

/* Target resolution failures, which require a started filter. */
static void test_unresolvable_target(void)
{
    tpw_filter_h handle = tpw_filter_create("tpw-test-link-target", noop_process_cb, NULL);
    TPW_ASSERT(handle != NULL);

    tpw_filter_port_h in = tpw_filter_add_audio_port(handle, TPW_FILTER_PORT_INPUT, &g_audio_cfg);
    TPW_ASSERT(in != NULL);
    TPW_ASSERT_EQ(tpw_filter_start(handle), TPW_STREAM_OK);

    /* Neither a bare node name nor its "node:port" form resolves. */
    TPW_ASSERT_EQ(tpw_filter_port_link(in, ABSENT_NODE), TPW_STREAM_ERR_NOT_FOUND);
    TPW_ASSERT_EQ(tpw_filter_port_link(in, ABSENT_NODE ":capture_FL"), TPW_STREAM_ERR_NOT_FOUND);
    /* An all-digit target is read as an object.serial; this one is nobody's. */
    TPW_ASSERT_EQ(tpw_filter_port_link(in, "4294967290"), TPW_STREAM_ERR_NOT_FOUND);

    /* A failed link leaves no partial state behind, so there is still
     * nothing to unlink. */
    TPW_ASSERT_EQ(tpw_filter_port_unlink(in), TPW_STREAM_ERR_NOT_CONFIGURED);

    tpw_filter_stop(handle, false);
    tpw_filter_destroy(handle);
}

/* Unlink's own state transitions, and a stop/start/destroy cycle over a
 * filter whose every link attempt failed. */
static void test_unlink_states(void)
{
    tpw_filter_h handle = tpw_filter_create("tpw-test-unlink", noop_process_cb, NULL);
    TPW_ASSERT(handle != NULL);

    tpw_filter_port_h in = tpw_filter_add_audio_port(handle, TPW_FILTER_PORT_INPUT, &g_audio_cfg);
    TPW_ASSERT(in != NULL);

    /* Never linked, before or after start. */
    TPW_ASSERT_EQ(tpw_filter_port_unlink(in), TPW_STREAM_ERR_NOT_CONFIGURED);
    TPW_ASSERT_EQ(tpw_filter_start(handle), TPW_STREAM_OK);
    TPW_ASSERT_EQ(tpw_filter_port_unlink(in), TPW_STREAM_ERR_NOT_CONFIGURED);

    /* A restart is clean, and linking is still rejected while stopped. */
    TPW_ASSERT_EQ(tpw_filter_stop(handle, false), TPW_STREAM_OK);
    TPW_ASSERT_EQ(tpw_filter_port_link(in, ABSENT_NODE), TPW_STREAM_ERR_NOT_CONFIGURED);
    TPW_ASSERT_EQ(tpw_filter_start(handle), TPW_STREAM_OK);
    TPW_ASSERT_EQ(tpw_filter_port_unlink(in), TPW_STREAM_ERR_NOT_CONFIGURED);

    tpw_filter_stop(handle, false);
    tpw_filter_destroy(handle);
}

/* Whitebox: drive the link's info callback directly to prove a link that
 * was up and then died is reported through the filter's error callback.
 * Physically removing a device is not something a unit test can do. */
static void test_source_unavailable_notification(void)
{
    tpw_filter_h handle = tpw_filter_create("tpw-test-link-notify", noop_process_cb, NULL);
    TPW_ASSERT(handle != NULL);
    TPW_ASSERT_EQ(tpw_filter_set_error_cb(handle, on_error), TPW_STREAM_OK);

    tpw_filter_port_h port_a = tpw_filter_add_audio_port(handle, TPW_FILTER_PORT_INPUT, &g_audio_cfg);
    tpw_filter_port_h port_b = tpw_filter_add_audio_port(handle, TPW_FILTER_PORT_INPUT, &g_audio_cfg);
    TPW_ASSERT(port_a != NULL && port_b != NULL);
    TPW_ASSERT_EQ(tpw_filter_start(handle), TPW_STREAM_OK);

    struct tpw_filter_port* a = (struct tpw_filter_port*)port_a;
    struct tpw_filter_port* b = (struct tpw_filter_port*)port_b;

    struct pw_link_info up = { .change_mask = PW_LINK_CHANGE_MASK_STATE, .state = PW_LINK_STATE_ACTIVE };
    struct pw_link_info gone = { .change_mask = PW_LINK_CHANGE_MASK_STATE, .state = PW_LINK_STATE_UNLINKED };
    struct pw_link_info no_state = { .change_mask = PW_LINK_CHANGE_MASK_PROPS, .state = PW_LINK_STATE_UNLINKED };

    /* A link that never came up going away is a failed negotiation, not a
     * lost source, so it must stay silent. */
    tpw_filter_link_on_info(b, &gone);
    TPW_ASSERT_EQ(g_error_calls, 0);

    /* Once a link has been up, losing it is a lost source. */
    tpw_filter_link_on_info(a, &up);
    TPW_ASSERT(a->link_state_seen_active);
    tpw_filter_link_on_info(a, &gone);

    TPW_ASSERT_EQ(g_error_calls, 1);
    TPW_ASSERT(g_last_port == port_a);
    TPW_ASSERT_EQ(g_last_error_code, TPW_STREAM_ERR_SOURCE_UNAVAILABLE);

    /* The port is no longer considered linked, so a repeat report is silent
     * and there is nothing left to unlink. */
    tpw_filter_link_on_info(a, &gone);
    TPW_ASSERT_EQ(g_error_calls, 1);
    TPW_ASSERT_EQ(tpw_filter_port_unlink(port_a), TPW_STREAM_ERR_NOT_CONFIGURED);

    /* An info event that carries no state change is ignored entirely. */
    tpw_filter_link_on_info(a, &up);
    tpw_filter_link_on_info(a, &no_state);
    TPW_ASSERT_EQ(g_error_calls, 1);

    tpw_filter_stop(handle, false);
    tpw_filter_destroy(handle);
}

int main(void)
{
    test_invalid_args();
    test_unresolvable_target();
    test_unlink_states();
    test_source_unavailable_notification();
    return 0;
}
