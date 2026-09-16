/* SPDX-License-Identifier: MIT */

/* The sensor-fusion bundle this library was built for, end to end: a camera
 * and a microphone linked as real nodes, plus a CPU temperature sensor that
 * is not a node at all — it is a sysfs file, so the application reads it and
 * pushes the value. A period hint pulls the cycle well below the camera's
 * frame interval, and the slow camera is then either held or absent
 * depending on its hold setting.
 *
 * Timing measurements make this unfit for the default suite, so it lives in
 * the hardware suite and exits 77 unless a camera, a microphone and a
 * readable temperature sensor are all present. */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <tpw/tpw_filter.h>

#include "tpw_test.h"
#include "tpw_test_hw_discover.h"

#define TEST_SKIP 77

/* Monotonic capture time in nanoseconds, the same clock domain
 * tpw_filter_port_buffer.pts otherwise carries from a real driver. A pushed
 * *block* only gets one pts for the whole buffer, not one per sample inside
 * it, so this records when the last sample in the block was read — good
 * enough to align the block against the camera's own per-frame pts, but not
 * a per-sample timestamp. A design that needs the latter would have to pack
 * a timestamp alongside each value instead of a bare float array. */
static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

#define RUN_MS 2000

/* A port keeps only its most recently pushed *buffer*, so anything pushed
 * more than once between two cycles is lost whole. The sensor is sampled
 * faster than the graph cycles, so readings are accumulated until a block
 * covers more than one cycle's worth: at roughly 1.6 ms a reading, five of
 * them span about 8 ms against a 5 ms cycle. A smaller block would still
 * lose data — batches of two and three measured 60% and 41% loss. */
#define SENSOR_PERIOD_US 1000
#define SENSOR_BATCH     5

/* A request, not a guarantee: PipeWire rounds the equivalent sample count
 * down to a power of two, so the real period lands at or below this. Well
 * under the camera's ~33 ms so most cycles fall between frames. */
#define HINT_NS (10 * 1000 * 1000)

static const char* const g_sensor_paths[] = {
    "/sys/class/thermal/thermal_zone0/temp",
    "/sys/class/hwmon/hwmon0/temp1_input",
    "/sys/class/hwmon/hwmon1/temp1_input",
    "/sys/class/hwmon/hwmon2/temp1_input",
};

/* Reads a millidegree-Celsius sysfs sensor. Returns false when the file is
 * absent or unreadable, which is how a machine without one is detected. */
static bool read_millidegrees(const char* path, long* out)
{
    FILE* f = fopen(path, "r");
    if (!f)
        return false;
    long v = 0;
    bool ok = fscanf(f, "%ld", &v) == 1;
    fclose(f);
    if (ok)
        *out = v;
    return ok;
}

static const char* find_sensor(void)
{
    long ignored;
    for (size_t i = 0; i < sizeof(g_sensor_paths) / sizeof(g_sensor_paths[0]); i++) {
        if (read_millidegrees(g_sensor_paths[i], &ignored))
            return g_sensor_paths[i];
    }
    return NULL;
}

struct bundle {
    const char* sensor;
    tpw_filter_h filter;
    tpw_filter_port_h video;
    tpw_filter_port_h mic;
    tpw_filter_port_h temp;

    unsigned cycles;
    unsigned video_fresh;   /* a new camera frame arrived */
    unsigned video_held;    /* the previous frame was re-presented */
    unsigned video_absent;  /* no frame at all this cycle */
    unsigned mic_cycles;
    unsigned temp_cycles;
    unsigned temp_values;   /* individual samples delivered, not cycles */
    int held_fd;
    float last_celsius;
    int64_t last_temp_pts;
    unsigned temp_pts_present;
    unsigned temp_pts_backwards; /* a later block reporting an earlier pts */

    volatile int sampling;
    unsigned sampled;       /* samples the sensor thread produced */
};

static void on_process(tpw_filter_h filter, tpw_filter_port_buffer* buffers, size_t n, void* user_data)
{
    struct bundle* b = user_data;
    (void)filter;
    b->cycles++;

    for (size_t i = 0; i < n; i++) {
        tpw_filter_port_buffer* buf = &buffers[i];

        if (buf->port == b->video) {
            tpw_dmabuf_plane plane;
            if (tpw_filter_port_get_dmabuf_planes(buf, &plane, 1) > 0) {
                if (buf->fresh) {
                    b->video_fresh++;
                } else {
                    b->video_held++;
                    b->held_fd = plane.fd;
                }
            } else {
                b->video_absent++;
            }
        } else if (buf->port == b->mic && buf->data && buf->size > 0) {
            b->mic_cycles++;
        } else if (buf->port == b->temp && buf->data && buf->size >= sizeof(float)) {
            b->temp_cycles++;
            /* A block, not a single reading: count every sample in it. */
            b->temp_values += (unsigned)(buf->size / sizeof(float));
            b->last_celsius = ((const float*)buf->data)[buf->size / sizeof(float) - 1];
            if (buf->pts >= 0) {
                b->temp_pts_present++;
                if (buf->pts < b->last_temp_pts)
                    b->temp_pts_backwards++;
                b->last_temp_pts = buf->pts;
            }
        }
    }
}

/* Samples the sysfs sensor faster than the graph cycles and hands over a
 * block at a time. A sensor with no PipeWire node joins a bundle this way,
 * and batching is what keeps its readings from being overwritten. */
static void* sensor_thread(void* arg)
{
    struct bundle* b = arg;
    float batch[SENSOR_BATCH];
    size_t n = 0;
    int64_t last_sample_pts = -1;

    while (b->sampling) {
        long milli = 0;
        if (read_millidegrees(b->sensor, &milli)) {
            batch[n++] = (float)milli / 1000.0f;
            last_sample_pts = now_ns();
            b->sampled++;
            if (n == SENSOR_BATCH) {
                /* The block's pts is when its last sample was read, not
                 * when the block happens to be delivered — the two can
                 * differ by up to a cycle if the push lands mid-cycle. */
                tpw_filter_push_port_data(b->filter, b->temp, batch, n * sizeof(float), last_sample_pts);
                n = 0;
            }
        }
        usleep(SENSOR_PERIOD_US);
    }
    return NULL;
}

/* Builds camera + mic + sensor into one filter and runs it. `hold` decides
 * what the video port reports on the cycles between camera frames. */
static void run_bundle(const char* camera, const char* mic, const char* sensor, bool hold,
                        struct bundle* out)
{
    struct bundle b = { .sensor = sensor, .held_fd = -1, .last_celsius = -1.0f };

    tpw_filter_h filter = tpw_filter_create("tpw-hw-bundle", on_process, &b);
    TPW_ASSERT(filter != NULL);

    tpw_video_config vcfg = { .width = 640, .height = 480, .pixel_format = "YUYV", .fps = 30 };
    tpw_filter_port_opts opts = { .memory = TPW_PORT_MEMORY_DMABUF };
    b.video = tpw_filter_add_video_port_ex(filter, TPW_FILTER_PORT_INPUT, &vcfg, &opts);
    b.mic = tpw_filter_add_signal_port(filter, TPW_FILTER_PORT_INPUT);
    b.temp = tpw_filter_add_signal_port(filter, TPW_FILTER_PORT_INPUT);
    TPW_ASSERT(b.video != NULL && b.mic != NULL && b.temp != NULL);

    if (hold)
        TPW_ASSERT_EQ(tpw_filter_port_set_hold(b.video, true), TPW_OK);
    TPW_ASSERT_EQ(tpw_filter_set_period_hint(filter, HINT_NS), TPW_OK);
    TPW_ASSERT_EQ(tpw_filter_start(filter), TPW_OK);

    TPW_ASSERT_EQ(tpw_filter_port_link(b.video, camera), TPW_OK);
    TPW_ASSERT_EQ(tpw_filter_port_link(b.mic, mic), TPW_OK);

    b.filter = filter;
    b.sampling = 1;
    pthread_t sampler;
    TPW_ASSERT_EQ(pthread_create(&sampler, NULL, sensor_thread, &b), 0);

    usleep(RUN_MS * 1000);

    b.sampling = 0;
    pthread_join(sampler, NULL);

    tpw_filter_stop(filter, false);
    tpw_filter_destroy(filter);
    *out = b;
}

int main(void)
{
    char camera[256], mic[256];
    const char* sensor = find_sensor();
    bool have_camera = tpw_test_find_node("Video/Source", camera, sizeof(camera));
    bool have_mic = tpw_test_find_node("Audio/Source", mic, sizeof(mic));

    if (!sensor || !have_camera || !have_mic) {
        printf("need a camera, a microphone and a temperature sensor; skipping\n");
        return TEST_SKIP;
    }
    printf("camera: %s\nmic   : %s\nsensor: %s\n", camera, mic, sensor);

    struct bundle held = { 0 };
    run_bundle(camera, mic, sensor, true, &held);
    double period_ms = held.cycles ? (double)RUN_MS / held.cycles : 0.0;
    printf("  hold on : %u cycles (%.2f ms)  video fresh=%u held=%u absent=%u  mic=%u\n", held.cycles,
           period_ms, held.video_fresh, held.video_held, held.video_absent, held.mic_cycles);
    printf("            sensor sampled=%u delivered=%u over %u cycles  %.1f C\n", held.sampled,
           held.temp_values, held.temp_cycles, (double)held.last_celsius);
    printf("            temp pts present=%u/%u  backwards=%u\n", held.temp_pts_present, held.temp_cycles,
           held.temp_pts_backwards);

    /* The hint applies even though the microphone, being hardware-clocked,
     * is what actually drives the graph. */
    TPW_ASSERT(held.cycles > 0);
    TPW_ASSERT(period_ms <= HINT_NS / 1000000.0);

    /* Batching is the point: the sensor runs faster than the cycle, so most
     * of its readings would be overwritten if pushed one at a time. Landing
     * anywhere near what was sampled means the blocks came through whole —
     * one-at-a-time pushing scores about a fifth of this. */
    TPW_ASSERT(held.sampled > 0);
    TPW_ASSERT(held.temp_values > held.sampled / 2);
    TPW_ASSERT(held.temp_values > held.temp_cycles); /* several samples per cycle */
    TPW_ASSERT(held.last_celsius > 0.0f && held.last_celsius < 150.0f);

    /* Every delivered block carries the capture time of its last sample,
     * and later blocks never report an earlier one. */
    TPW_ASSERT_EQ(held.temp_pts_present, held.temp_cycles);
    TPW_ASSERT_EQ(held.temp_pts_backwards, (unsigned)0);

    /* The microphone drives, so it delivers on nearly every cycle. */
    TPW_ASSERT(held.mic_cycles > held.cycles / 2);

    /* The camera is far slower than the cycle, so most cycles fall between
     * its frames and re-present the previous frame's descriptor. */
    if (held.video_fresh > 0) {
        TPW_ASSERT(held.video_held > held.video_fresh);
        TPW_ASSERT(held.held_fd >= 0);
    } else {
        printf("  note: camera linked but negotiated no DMABUF frames\n");
    }

    /* Same bundle without hold: the gaps become empty instead of repeating
     * the last frame. */
    struct bundle plain = { 0 };
    run_bundle(camera, mic, sensor, false, &plain);
    printf("  hold off: %u cycles  video fresh=%u held=%u absent=%u\n", plain.cycles, plain.video_fresh,
           plain.video_held, plain.video_absent);

    TPW_ASSERT_EQ(plain.video_held, (unsigned)0);
    if (plain.video_fresh > 0)
        TPW_ASSERT(plain.video_absent > 0);

    return 0;
}
