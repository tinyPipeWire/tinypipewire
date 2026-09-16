/* SPDX-License-Identifier: MIT */

/* A role is a hint no daemon checks, so what can be verified without a
 * session manager is that it lands on the node, and only when asked for. */

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

static const tpw_audio_config cfg = { .sample_rate = 48000, .channels = 2 };

/* Reads media.role off the connected node, copying it out under the lock. */
static bool node_role(tpw_stream_h handle, char* out, size_t out_size)
{
    struct tpw_stream* stream = (struct tpw_stream*)handle;
    pw_thread_loop_lock(stream->conn.loop);
    const struct pw_properties* props = pw_stream_get_properties(stream->pw_stream);
    const char* role = props ? pw_properties_get(props, PW_KEY_MEDIA_ROLE) : NULL;
    if (role)
        snprintf(out, out_size, "%s", role);
    pw_thread_loop_unlock(stream->conn.loop);
    return role != NULL;
}

/* A role can be set, replaced and cleared, and NULL and "" both clear it. */
static void test_set_and_clear(void)
{
    TPW_ASSERT_EQ(tpw_stream_set_role(NULL, "Music"), TPW_ERR_INVALID_ARG);

    tpw_stream_h s = tpw_stream_create(TPW_STREAM_TYPE_AUDIO, on_data, NULL);
    TPW_ASSERT(s != NULL);
    struct tpw_stream* stream = (struct tpw_stream*)s;
    TPW_ASSERT(stream->role == NULL); /* no role until told otherwise */

    TPW_ASSERT_EQ(tpw_stream_set_role(s, "Music"), TPW_OK);
    TPW_ASSERT_EQ(strcmp(stream->role, "Music"), 0);
    TPW_ASSERT_EQ(tpw_stream_set_role(s, "Communication"), TPW_OK);
    TPW_ASSERT_EQ(strcmp(stream->role, "Communication"), 0);

    TPW_ASSERT_EQ(tpw_stream_set_role(s, NULL), TPW_OK);
    TPW_ASSERT(stream->role == NULL);
    TPW_ASSERT_EQ(tpw_stream_set_role(s, "Music"), TPW_OK);
    TPW_ASSERT_EQ(tpw_stream_set_role(s, ""), TPW_OK);
    TPW_ASSERT(stream->role == NULL);

    tpw_stream_destroy(s);
}

/* Unlike a target, a role does not contradict manual wiring, so neither
 * order of the two setters is refused. */
static void test_accepted_with_autoconnect_off(void)
{
    tpw_stream_h s = tpw_stream_create(TPW_STREAM_TYPE_AUDIO, on_data, NULL);
    TPW_ASSERT(s != NULL);
    TPW_ASSERT_EQ(tpw_stream_set_autoconnect(s, false), TPW_OK);
    TPW_ASSERT_EQ(tpw_stream_set_role(s, "Music"), TPW_OK);
    tpw_stream_destroy(s);

    s = tpw_stream_create(TPW_STREAM_TYPE_AUDIO, on_data, NULL);
    TPW_ASSERT(s != NULL);
    TPW_ASSERT_EQ(tpw_stream_set_role(s, "Music"), TPW_OK);
    TPW_ASSERT_EQ(tpw_stream_set_autoconnect(s, false), TPW_OK);
    tpw_stream_destroy(s);
}

/* The role reaches the node the format connects, on capture and playback
 * alike, and a stream that set none declares none. */
static void test_role_reaches_the_node(void)
{
    char role[64];

    tpw_stream_h s = tpw_stream_create(TPW_STREAM_TYPE_AUDIO, on_data, NULL);
    TPW_ASSERT(s != NULL);
    TPW_ASSERT_EQ(tpw_stream_set_audio_config(s, &cfg), TPW_OK);
    TPW_ASSERT(!node_role(s, role, sizeof(role)));
    tpw_stream_destroy(s);

    s = tpw_stream_create(TPW_STREAM_TYPE_AUDIO, on_data, NULL);
    TPW_ASSERT(s != NULL);
    TPW_ASSERT_EQ(tpw_stream_set_role(s, "Communication"), TPW_OK);
    TPW_ASSERT_EQ(tpw_stream_set_audio_config(s, &cfg), TPW_OK);
    TPW_ASSERT(node_role(s, role, sizeof(role)));
    TPW_ASSERT_EQ(strcmp(role, "Communication"), 0);
    tpw_stream_destroy(s);

    tpw_stream_h p = tpw_stream_create_playback(on_fill, NULL);
    TPW_ASSERT(p != NULL);
    TPW_ASSERT_EQ(tpw_stream_set_role(p, "Music"), TPW_OK);
    TPW_ASSERT_EQ(tpw_stream_set_audio_config(p, &cfg), TPW_OK);
    TPW_ASSERT(node_role(p, role, sizeof(role)));
    TPW_ASSERT_EQ(strcmp(role, "Music"), 0);
    tpw_stream_destroy(p);
}

int main(void)
{
    test_set_and_clear();
    test_accepted_with_autoconnect_off();
    test_role_reaches_the_node();
    printf("test_stream_role: all cases passed\n");
    return 0;
}
