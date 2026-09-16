/* SPDX-License-Identifier: MIT */

#include <spa/buffer/buffer.h>
#include <spa/param/buffers.h>
#include <spa/pod/iter.h>

#include "tpw/tpw_filter.h"
#include "tpw_filter_internal.h"     /* whitebox: feed a synthetic DMABUF buffer */
#include "tpw_spa_format_internal.h" /* whitebox: verify the Buffers POD */
#include "tpw_test.h"

static void noop_process_cb(tpw_filter_h filter, tpw_filter_port_buffer* buffers, size_t n_buffers,
                             void* user_data)
{
    (void)filter;
    (void)buffers;
    (void)n_buffers;
    (void)user_data;
}

/* Feeds a synthetic buffer to the accessor and checks it extracts each
 * plane's fd/offset/stride/size, skips non-DMABUF blocks, falls back to
 * maxsize when a block has no chunk, and clamps writes to max_planes while
 * still returning the true plane count. */
static void test_plane_extraction(tpw_filter_port_h dmabuf_port)
{
    struct tpw_filter_port* port = (struct tpw_filter_port*)dmabuf_port;

    struct spa_chunk chunk0 = { .offset = 0, .size = 100, .stride = 640 };
    struct spa_data datas[3] = {
        { .type = SPA_DATA_DmaBuf, .fd = 42, .mapoffset = 16, .maxsize = 200, .chunk = &chunk0 },
        { .type = SPA_DATA_MemPtr, .fd = -1 },                            /* must be skipped */
        { .type = SPA_DATA_DmaBuf, .fd = 43, .mapoffset = 0, .maxsize = 50, .chunk = NULL },
    };
    struct spa_buffer sb = { .n_datas = 3, .datas = datas };
    port->current_dmabuf_buf = &sb;

    tpw_dmabuf_plane planes[4];
    tpw_filter_port_buffer buf = { .port = dmabuf_port };
    TPW_ASSERT_EQ(tpw_filter_port_get_dmabuf_planes(&buf, planes, 4), (size_t)2); /* MemPtr skipped */
    TPW_ASSERT_EQ(planes[0].fd, 42);
    TPW_ASSERT_EQ(planes[0].offset, 16u);
    TPW_ASSERT_EQ(planes[0].stride, 640u);
    TPW_ASSERT_EQ(planes[0].size, 100u);   /* from chunk->size */
    TPW_ASSERT_EQ(planes[1].fd, 43);
    TPW_ASSERT_EQ(planes[1].stride, 0u);   /* no chunk -> stride 0 */
    TPW_ASSERT_EQ(planes[1].size, 50u);    /* no chunk -> maxsize fallback */

    /* max_planes bounds how many are written, not the returned count. */
    tpw_dmabuf_plane one;
    TPW_ASSERT_EQ(tpw_filter_port_get_dmabuf_planes(&buf, &one, 1), (size_t)2);
    TPW_ASSERT_EQ(one.fd, 42);

    port->current_dmabuf_buf = NULL;
}

/* The DMABUF Buffers param must advertise DmaBuf and nothing else. */
static void test_dmabuf_buffers_pod(void)
{
    uint8_t storage[512];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
    const struct spa_pod* pod = tpw_spa_build_dmabuf_buffers(&b, 0);
    TPW_ASSERT(pod != NULL);

    const struct spa_pod_prop* dt = spa_pod_find_prop(pod, NULL, SPA_PARAM_BUFFERS_dataType);
    TPW_ASSERT(dt != NULL);
    int32_t data_type = 0;
    TPW_ASSERT_EQ(spa_pod_get_int(&dt->value, &data_type), 0);
    TPW_ASSERT_EQ(data_type, 1 << SPA_DATA_DmaBuf);

    /* A pool count is requested so hold can retain a frame. */
    TPW_ASSERT(spa_pod_find_prop(pod, NULL, SPA_PARAM_BUFFERS_buffers) != NULL);
}

int main(void)
{
    test_dmabuf_buffers_pod();

    tpw_filter_h filter = tpw_filter_create("tpw-test-dmabuf", noop_process_cb, NULL);
    TPW_ASSERT(filter != NULL);

    tpw_video_config cfg = { .width = 640, .height = 480, .pixel_format = "RGB", .fps = 30 };
    tpw_filter_port_opts dmabuf_opts = { .memory = TPW_PORT_MEMORY_DMABUF };

    /* DMABUF is accepted on a video INPUT port. */
    tpw_filter_port_h dmabuf_in =
        tpw_filter_add_video_port_ex(filter, TPW_FILTER_PORT_INPUT, &cfg, &dmabuf_opts);
    TPW_ASSERT(dmabuf_in != NULL);
    TPW_ASSERT_EQ(tpw_filter_port_get_type(dmabuf_in), TPW_STREAM_TYPE_VIDEO);

    /* DMABUF is import-only: refused on an OUTPUT port. */
    TPW_ASSERT(tpw_filter_add_video_port_ex(filter, TPW_FILTER_PORT_OUTPUT, &cfg, &dmabuf_opts) == NULL);

    /* MJPEG and H.264 frames are never handed out as DMABUF, even on an INPUT port. */
    tpw_video_config mjpg_cfg = { .width = 640, .height = 480, .pixel_format = "MJPG", .fps = 30 };
    TPW_ASSERT(tpw_filter_add_video_port_ex(filter, TPW_FILTER_PORT_INPUT, &mjpg_cfg, &dmabuf_opts) == NULL);

    tpw_video_config h264_cfg = { .width = 640, .height = 480, .pixel_format = "H264", .fps = 30 };
    TPW_ASSERT(tpw_filter_add_video_port_ex(filter, TPW_FILTER_PORT_INPUT, &h264_cfg, &dmabuf_opts) == NULL);

    /* opts == NULL is exactly the non-_ex call: a normal CPU video port. */
    tpw_filter_port_h cpu_in = tpw_filter_add_video_port_ex(filter, TPW_FILTER_PORT_INPUT, &cfg, NULL);
    TPW_ASSERT(cpu_in != NULL);

    /* The accessor never fabricates a plane: it returns 0 for a CPU port,
     * for a DMABUF port outside a cycle (no current buffer), and for NULL. */
    tpw_dmabuf_plane planes[4];
    tpw_filter_port_buffer cpu_buf = { .port = cpu_in };
    TPW_ASSERT_EQ(tpw_filter_port_get_dmabuf_planes(&cpu_buf, planes, 4), 0);

    tpw_filter_port_buffer dmabuf_buf = { .port = dmabuf_in };
    TPW_ASSERT_EQ(tpw_filter_port_get_dmabuf_planes(&dmabuf_buf, planes, 4), 0);

    TPW_ASSERT_EQ(tpw_filter_port_get_dmabuf_planes(NULL, planes, 4), 0);

    /* With a real DMABUF frame present, the accessor extracts its planes. */
    test_plane_extraction(dmabuf_in);

    /* Configuration setters reject bad handles and directions. */
    tpw_filter_port_h out = tpw_filter_add_video_port(filter, TPW_FILTER_PORT_OUTPUT, &cfg);
    TPW_ASSERT(out != NULL);
    TPW_ASSERT_EQ(tpw_filter_port_set_hold(out, true), TPW_ERR_INVALID_ARG);   /* output port */
    TPW_ASSERT_EQ(tpw_filter_port_set_hold(NULL, true), TPW_ERR_INVALID_ARG);
    TPW_ASSERT_EQ(tpw_filter_set_period_hint(NULL, 1000000), TPW_ERR_INVALID_ARG);

    tpw_filter_destroy(filter);
    return 0;
}
