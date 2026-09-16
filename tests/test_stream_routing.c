/* SPDX-License-Identifier: MIT */

/* The routing rules are argument and state checking, and the pairing is
 * arithmetic, so both are exercised without a device. tpw_stream_pair_ports()
 * is reachable directly, which is what keeps the risky part testable — the
 * same separation that made the playback fill rules testable. */

#include <string.h>

#include "tpw_stream_internal.h"
#include "tpw_test.h"

static void on_data(tpw_stream_h stream, const tpw_stream_buffer* buf, void* user_data)
{
    (void)stream;
    (void)buf;
    (void)user_data;
}

static void on_fill(tpw_stream_h stream, tpw_stream_playback_buffer* buf, void* user_data)
{
    (void)stream;
    (void)user_data;
    buf->size = 0;
}

static tpw_stream_h make_capture(void)
{
    tpw_stream_h s = tpw_stream_create(TPW_DATA_AUDIO, on_data, NULL);
    TPW_ASSERT(s != NULL);
    return s;
}

/* Pairing: equal counts pair straight through, a short target is refused
 * outright, and a longer one reports what it left over. */
static void test_pairing(void)
{
    size_t surplus = 12345;

    TPW_ASSERT_EQ(tpw_stream_pair_ports(2, 2, &surplus), (size_t)2);
    TPW_ASSERT_EQ(surplus, (size_t)0);

    TPW_ASSERT_EQ(tpw_stream_pair_ports(1, 1, &surplus), (size_t)1);
    TPW_ASSERT_EQ(surplus, (size_t)0);

    /* stereo stream, mono device — refused whole, never half-wired */
    TPW_ASSERT_EQ(tpw_stream_pair_ports(2, 1, &surplus), (size_t)0);
    TPW_ASSERT_EQ(surplus, (size_t)0);

    /* mono stream, stereo device — links one, reports the other */
    TPW_ASSERT_EQ(tpw_stream_pair_ports(1, 2, &surplus), (size_t)1);
    TPW_ASSERT_EQ(surplus, (size_t)1);

    TPW_ASSERT_EQ(tpw_stream_pair_ports(2, 8, &surplus), (size_t)2);
    TPW_ASSERT_EQ(surplus, (size_t)6);

    /* a stream with no ports has nothing to pair */
    TPW_ASSERT_EQ(tpw_stream_pair_ports(0, 2, &surplus), (size_t)0);

    /* the surplus pointer is optional */
    TPW_ASSERT_EQ(tpw_stream_pair_ports(2, 2, NULL), (size_t)2);
}

/* A stream that declares nothing is wired by the session manager, as before. */
static void test_autoconnect_is_the_default(void)
{
    tpw_stream_h s = make_capture();
    TPW_ASSERT(((struct tpw_stream*)s)->autoconnect);
    tpw_stream_destroy(s);

    tpw_stream_h p = tpw_stream_create_playback(on_fill, NULL);
    TPW_ASSERT(p != NULL);
    TPW_ASSERT(((struct tpw_stream*)p)->autoconnect);
    tpw_stream_destroy(p);
}

/* The mode is fixed once the format has connected the stream. Refusing this
 * is not a formality: by then the node exists with its autoconnect property
 * already set, and a session manager may have wired it, so accepting the
 * change would tell the caller something untrue. */
static void test_mode_is_fixed_after_connect(void)
{
    tpw_stream_h s = make_capture();
    tpw_audio_config cfg = { .sample_rate = 48000, .channels = 2 };

    TPW_ASSERT_EQ(tpw_stream_set_autoconnect(s, false), TPW_OK);
    TPW_ASSERT_EQ(tpw_stream_set_audio_config(s, &cfg), TPW_OK);
    TPW_ASSERT_EQ(tpw_stream_set_autoconnect(s, true), TPW_ERR_INVALID_ARG);

    tpw_stream_destroy(s);
}

/* Video connects through a different config call, so it gets the same rule
 * checked separately — in both directions, since the mode is fixed by the
 * stream being connected rather than by which way it was set. */
static void test_mode_is_fixed_after_video_connect(void)
{
    tpw_stream_h s = tpw_stream_create(TPW_DATA_VIDEO, on_data, NULL);
    TPW_ASSERT(s != NULL);
    tpw_video_config cfg = { .width = 640, .height = 480, .pixel_format = "YUYV", .fps = 30 };

    TPW_ASSERT_EQ(tpw_stream_set_video_config(s, &cfg), TPW_OK);
    TPW_ASSERT_EQ(tpw_stream_set_autoconnect(s, false), TPW_ERR_INVALID_ARG);
    TPW_ASSERT_EQ(tpw_stream_set_autoconnect(s, true), TPW_ERR_INVALID_ARG);
    TPW_ASSERT(((struct tpw_stream*)s)->autoconnect); /* unchanged by the refusals */

    tpw_stream_destroy(s);
}

/* The target hint and manual wiring are exclusive in BOTH call orders. */
static void test_hint_and_manual_are_exclusive(void)
{
    tpw_stream_h a = make_capture();
    TPW_ASSERT_EQ(tpw_stream_set_target(a, "some-device"), TPW_OK);
    TPW_ASSERT_EQ(tpw_stream_set_autoconnect(a, false), TPW_ERR_INVALID_ARG);
    TPW_ASSERT(((struct tpw_stream*)a)->autoconnect); /* unchanged by the refusal */
    tpw_stream_destroy(a);

    tpw_stream_h b = make_capture();
    TPW_ASSERT_EQ(tpw_stream_set_autoconnect(b, false), TPW_OK);
    TPW_ASSERT_EQ(tpw_stream_set_target(b, "some-device"), TPW_ERR_INVALID_ARG);
    TPW_ASSERT(((struct tpw_stream*)b)->target == NULL); /* unchanged by the refusal */
    /* Clearing a target is not naming one, so it stays allowed. */
    TPW_ASSERT_EQ(tpw_stream_set_target(b, NULL), TPW_OK);
    tpw_stream_destroy(b);
}

/* Linking is refused before start and while the session manager still owns
 * the wiring. */
static void test_link_ordering_and_mode(void)
{
    tpw_stream_h auto_s = make_capture();
    tpw_audio_config cfg = { .sample_rate = 48000, .channels = 2 };
    TPW_ASSERT_EQ(tpw_stream_set_audio_config(auto_s, &cfg), TPW_OK);
    /* autoconnect is on: refused on the mode, before anything is looked up */
    TPW_ASSERT_EQ(tpw_stream_link(auto_s, "some-device"), TPW_ERR_INVALID_ARG);
    tpw_stream_destroy(auto_s);

    tpw_stream_h s = make_capture();
    TPW_ASSERT_EQ(tpw_stream_set_autoconnect(s, false), TPW_OK);
    /* before the format, and before start */
    TPW_ASSERT_EQ(tpw_stream_link(s, "some-device"), TPW_ERR_NOT_CONFIGURED);
    TPW_ASSERT_EQ(tpw_stream_set_audio_config(s, &cfg), TPW_OK);
    TPW_ASSERT_EQ(tpw_stream_link(s, "some-device"), TPW_ERR_NOT_CONFIGURED);

    /* argument checking does not depend on the graph either */
    TPW_ASSERT_EQ(tpw_stream_link(s, NULL), TPW_ERR_INVALID_ARG);
    TPW_ASSERT_EQ(tpw_stream_link(s, ""), TPW_ERR_INVALID_ARG);

    tpw_stream_destroy(s);
}

/* Releasing what was never wired is a caller mistake, not a no-op. */
static void test_unlink_without_links_is_refused(void)
{
    tpw_stream_h s = make_capture();
    TPW_ASSERT_EQ(tpw_stream_unlink(s), TPW_ERR_NOT_CONFIGURED);

    TPW_ASSERT_EQ(tpw_stream_set_autoconnect(s, false), TPW_OK);
    TPW_ASSERT_EQ(tpw_stream_unlink(s), TPW_ERR_NOT_CONFIGURED);

    tpw_stream_destroy(s);
}

/* A link waits for the stream's ports and then its target. Without a session manager the ports
 * never come and the wait times out, and with one the absent target is not found. */
static void test_link_reports_timeout_or_missing_target(void)
{
    tpw_stream_h s = make_capture();
    tpw_audio_config cfg = { .sample_rate = 48000, .channels = 2 };
    TPW_ASSERT_EQ(tpw_stream_set_autoconnect(s, false), TPW_OK);
    TPW_ASSERT_EQ(tpw_stream_set_audio_config(s, &cfg), TPW_OK);
    TPW_ASSERT_EQ(tpw_stream_start(s), TPW_OK);

    int res = tpw_stream_link(s, "tpw-test-no-such-node");
    TPW_ASSERT(res == TPW_ERR_TIMEOUT || res == TPW_ERR_NOT_FOUND);

    tpw_stream_destroy(s);
}

/* Opting out and never naming a device is a legitimate state: the stream runs
 * and simply carries nothing. */
static void test_opted_out_and_unlinked_runs(void)
{
    tpw_stream_h s = make_capture();
    tpw_audio_config cfg = { .sample_rate = 48000, .channels = 2 };

    TPW_ASSERT_EQ(tpw_stream_set_autoconnect(s, false), TPW_OK);
    TPW_ASSERT_EQ(tpw_stream_set_audio_config(s, &cfg), TPW_OK);
    TPW_ASSERT_EQ(tpw_stream_start(s), TPW_OK);
    TPW_ASSERT(((struct tpw_stream*)s)->links == NULL);
    TPW_ASSERT_EQ(tpw_stream_stop(s, false), TPW_OK);

    tpw_stream_destroy(s);
}

/* Works right after create(), before any format is set, and a NULL `out`
 * is a safe count-only query — no assumption about what devices exist. */
static void test_get_target_list(void)
{
    size_t count = 99;
    TPW_ASSERT_EQ(tpw_stream_get_target_list(NULL, NULL, 0, &count), TPW_ERR_INVALID_ARG);
    TPW_ASSERT_EQ(count, (size_t)0);

    tpw_stream_h s = make_capture();
    TPW_ASSERT_EQ(tpw_stream_get_target_list(s, NULL, 0, NULL), TPW_ERR_INVALID_ARG);
    TPW_ASSERT_EQ(tpw_stream_get_target_list(s, NULL, 0, &count), TPW_OK);

    tpw_target_info targets[8];
    size_t again = 0;
    TPW_ASSERT_EQ(tpw_stream_get_target_list(s, targets, 8, &again), TPW_OK);
    TPW_ASSERT_EQ(again, count);
    for (size_t i = 0; i < again && i < 8; i++)
        TPW_ASSERT(targets[i].name[0] != '\0');

    tpw_stream_destroy(s);
}

/* Releasing an unlinked stream is safe to call from teardown paths. */
static void test_release_is_safe_when_unlinked(void)
{
    tpw_stream_h s = make_capture();
    tpw_stream_release_links((struct tpw_stream*)s); /* must not crash */
    TPW_ASSERT(((struct tpw_stream*)s)->links == NULL);
    tpw_stream_destroy(s);

    tpw_stream_release_links(NULL); /* nor on nothing at all */
}

int main(void)
{
    test_pairing();
    test_autoconnect_is_the_default();
    test_mode_is_fixed_after_connect();
    test_mode_is_fixed_after_video_connect();
    test_hint_and_manual_are_exclusive();
    test_link_ordering_and_mode();
    test_unlink_without_links_is_refused();
    test_opted_out_and_unlinked_runs();
    test_link_reports_timeout_or_missing_target();
    test_release_is_safe_when_unlinked();
    test_get_target_list();
    printf("test_stream_routing: all cases passed\n");
    return 0;
}
