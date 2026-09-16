/* SPDX-License-Identifier: MIT */

/* A filter with nothing but a pushed sensor on it — no camera, no
 * microphone, no links at all. There is no hardware-clocked node to drive
 * the graph, so PipeWire schedules it on its Dummy-Driver instead, and the
 * period hint is what decides how often. This is the arrangement an
 * application gets when its only source is something PipeWire has no node
 * for, such as a sysfs sensor.
 *
 * Timing measurements make this unfit for the default suite, so it lives in
 * the hardware suite and exits 77 when no temperature sensor is readable. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <tpw/tpw_filter.h>

#include "tpw_test.h"

#define TEST_SKIP 77

/* Monotonic capture time in nanoseconds, the same clock domain
 * tpw_filter_port_buffer.pts otherwise carries from a real driver. One
 * reading per cycle here, so this is the exact sample time — no batching
 * blurs it the way it does in test_filter_sensor_latency_hw. */
static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

#define RUN_MS 1500

/* A request, not a guarantee: PipeWire rounds the equivalent sample count
 * down to a power of two, so the real period lands at or below this. */
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

struct run {
    const char* sensor;
    tpw_filter_port_h port;
    unsigned cycles;
    unsigned with_data;
    float last_celsius;
    int64_t last_pts;
    unsigned pts_present;
    unsigned pts_backwards; /* a later cycle reporting an earlier pts */
};

static void on_process(tpw_filter_h filter, tpw_filter_port_buffer* buffers, size_t n, void* user_data)
{
    struct run* r = user_data;
    r->cycles++;

    for (size_t i = 0; i < n; i++) {
        if (buffers[i].port != r->port || !buffers[i].data || buffers[i].size < sizeof(float))
            continue;
        r->last_celsius = *(const float*)buffers[i].data;
        r->with_data++;
        if (buffers[i].pts >= 0) {
            r->pts_present++;
            if (buffers[i].pts < r->last_pts)
                r->pts_backwards++;
            r->last_pts = buffers[i].pts;
        }
    }

    /* Sampling from inside the callback keeps one reading per cycle by
     * construction, so nothing is overwritten and no batching is needed —
     * the pts pushed here is exactly when this one value was read. */
    long milli = 0;
    if (read_millidegrees(r->sensor, &milli)) {
        float celsius = (float)milli / 1000.0f;
        tpw_filter_push_port_data(filter, r->port, &celsius, sizeof(celsius), now_ns());
    }
}

/* Runs a sensor-only filter for RUN_MS. `hint_ns` of 0 leaves the graph at
 * its default period. */
static void measure(const char* sensor, uint32_t hint_ns, struct run* out)
{
    struct run r = { .sensor = sensor, .last_celsius = -1.0f };

    tpw_filter_h filter = tpw_filter_create("tpw-hw-sensor-dummy", on_process, &r);
    TPW_ASSERT(filter != NULL);

    r.port = tpw_filter_add_signal_port(filter, TPW_FILTER_PORT_INPUT);
    TPW_ASSERT(r.port != NULL);

    if (hint_ns > 0)
        TPW_ASSERT_EQ(tpw_filter_set_period_hint(filter, hint_ns), TPW_OK);

    TPW_ASSERT_EQ(tpw_filter_start(filter), TPW_OK);
    usleep(RUN_MS * 1000);
    tpw_filter_stop(filter, false);
    tpw_filter_destroy(filter);

    *out = r;
}

int main(void)
{
    const char* sensor = find_sensor();
    if (!sensor) {
        printf("no readable temperature sensor; skipping\n");
        return TEST_SKIP;
    }

    long milli = 0;
    TPW_ASSERT(read_millidegrees(sensor, &milli));
    printf("sensor: %s (%.1f C)\n", sensor, milli / 1000.0);

    struct run base = { 0 }, fast = { 0 };
    measure(sensor, 0, &base);
    measure(sensor, HINT_NS, &fast);

    double base_ms = base.cycles ? (double)RUN_MS / base.cycles : 0.0;
    double fast_ms = fast.cycles ? (double)RUN_MS / fast.cycles : 0.0;
    printf("  default: %u cycles, %.2f ms/cycle\n", base.cycles, base_ms);
    printf("  hinted : %u cycles, %.2f ms/cycle (asked for %.0f ms)\n", fast.cycles, fast_ms,
           HINT_NS / 1000000.0);

    /* Nothing is linked, so the callback only runs at all because PipeWire
     * put the filter on its Dummy-Driver. */
    TPW_ASSERT(base.cycles > 0);
    TPW_ASSERT(fast.cycles > 0);

    /* Pushed data still reaches the callback with no graph source present.
     * The first cycle runs before anything has been staged. */
    TPW_ASSERT(base.with_data + 1 >= base.cycles);
    TPW_ASSERT(fast.with_data + 1 >= fast.cycles);
    TPW_ASSERT(fast.last_celsius > 0.0f && fast.last_celsius < 150.0f);

    /* One reading per cycle carries its own exact capture time: pts arrives
     * with every delivered value, and later cycles never report an earlier
     * one, since each push happens strictly after the previous cycle's. */
    TPW_ASSERT_EQ(fast.pts_present, fast.with_data);
    TPW_ASSERT_EQ(fast.pts_backwards, (unsigned)0);

    /* The hint drives the Dummy-Driver exactly as it drives a hardware one.
     * It is an upper bound, so only tighten when the default left room — a
     * machine already running a small quantum has none. */
    TPW_ASSERT(fast_ms <= base_ms * 1.2);
    if (base_ms > HINT_NS / 1000000.0) {
        TPW_ASSERT(fast_ms <= HINT_NS / 1000000.0);
        TPW_ASSERT(fast.cycles > base.cycles);
    }

    return 0;
}
