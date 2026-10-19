#include "time_sync.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define EPOCH UINT64_C(1800000000000000)
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); return 0; \
} } while (0)

static int no_anchor_and_single(void)
{
    tc_clock c, saved;
    tc_report r;
    uint64_t utc = 123;
    uint32_t error = 456;
    tc_init(&c); saved = c;
    CHECK(!tc_estimate(&c, 0, &utc, &error));
    CHECK(utc == 123 && error == 456);
    CHECK(!tc_get_report(&c, &r));
    CHECK(!tc_anchor(&c, 0, 0, 0, 0));
    CHECK(memcmp(&saved, &c, sizeof(c)) == 0);
    CHECK(tc_anchor(&c, 1000000, EPOCH + 1000000, 20, 401));
    CHECK(tc_get_report(&c, &r));
    CHECK(r.count == 1 && r.offset_us == (int64_t)EPOCH && r.drift_ppm == 0);
    CHECK(r.roundtrip_mean_us == 401 && r.roundtrip_max_us == 401);
    CHECK(tc_estimate(&c, 1000000, &utc, &error));
    CHECK(utc == EPOCH + 1000000 && error >= 222 && error <= 225);
    CHECK(tc_estimate(&c, 11000000, &utc, &error));
    CHECK(utc == EPOCH + 11000000 && error >= 5222 && error <= 5225);
    utc = 123; error = 456;
    CHECK(!tc_estimate(&c, 999999, &utc, &error));
    CHECK(!tc_estimate(&c, 11000001, &utc, &error));
    CHECK(utc == 123 && error == 456);
    CHECK(!tc_estimate(NULL, 0, &utc, &error));
    CHECK(!tc_estimate(&c, 1000000, NULL, &error));
    CHECK(!tc_estimate(&c, 1000000, &utc, NULL));
    CHECK(!tc_anchor(NULL, 0, EPOCH, 0, 0));
    tc_init(NULL);
    return 1;
}

static int known_drift_and_ring(void)
{
    tc_clock c;
    tc_report r;
    uint64_t utc;
    uint32_t error, i;
    tc_init(&c);
    for (i = 0; i < 20; ++i) {
        uint64_t boot = UINT64_C(1000000) + (uint64_t)i * 1000000;
        CHECK(tc_anchor(&c, boot, EPOCH + boot + i * 50, 20 + i, 100 + i * 10));
    }
    CHECK(tc_get_report(&c, &r));
    CHECK(r.count == 16 && r.oldest_boot_us == 5000000 && r.last_boot_us == 20000000);
    CHECK(r.last_utc_us == EPOCH + 20000000 + 950);
    CHECK(r.offset_us == (int64_t)EPOCH + 950);
    CHECK(fabs(r.drift_ppm - 50) < 1e-9);
    CHECK(r.residual_std_us < 1e-9 && r.max_abs_residual_us < 1e-9);
    CHECK(r.roundtrip_max_us == 290 && r.roundtrip_mean_us == 215);
    CHECK(r.anchor_uncert_max_us == 39);
    CHECK(tc_estimate(&c, 9500000, &utc, &error));
    CHECK(utc == EPOCH + 9500000 + 425);
    CHECK(error >= 434 && error <= 440);
    CHECK(!tc_estimate(&c, 4999999, &utc, &error));
    CHECK(tc_estimate(&c, 30000000, &utc, &error));
    CHECK(utc == EPOCH + 30000000 + 1450);
    CHECK(!tc_estimate(&c, 30000001, &utc, &error));
    return 1;
}

static int quantization_and_recovery(void)
{
    tc_clock c;
    tc_report r;
    uint64_t utc;
    uint32_t error, i;
    const int quantized[4] = {-500, 0, 500, 0};
    tc_init(&c);
    for (i = 0; i < 20; ++i) {
        uint64_t boot = UINT64_C(1000000) + (uint64_t)i * 2000000;
        uint64_t ideal = EPOCH + boot + i * 100;
        uint64_t value = quantized[i % 4] < 0 ? ideal - 500 :
                         ideal + (uint64_t)quantized[i % 4];
        CHECK(tc_anchor(&c, boot, value, 1000, 1501));
    }
    CHECK(tc_get_report(&c, &r));
    CHECK(r.residual_std_us > 300 && r.residual_std_us < 500);
    CHECK(r.max_abs_residual_us > 400 && r.max_abs_residual_us < 600);
    CHECK(fabs(r.drift_ppm - 50) < 20);
    CHECK(tc_estimate(&c, r.last_boot_us, &utc, &error));
    CHECK(error > 2200 && error < 2400);
    CHECK(!tc_estimate(&c, r.last_boot_us + 10000001, &utc, &error));
    /* A fresh GPS anchor recovers after a period with no GPS updates. */
    CHECK(tc_anchor(&c, 99000000, EPOCH + 99000000 + 4900, 1000, 1501));
    CHECK(tc_estimate(&c, 99000000, &utc, &error));
    return 1;
}

static int rejection_is_atomic(void)
{
    tc_clock c, saved;
    uint64_t utc;
    uint32_t error;
    tc_init(&c);
    CHECK(tc_anchor(&c, 1000000, EPOCH + 1000000, 10, 100));
    CHECK(tc_anchor(&c, 2000000, EPOCH + 2000050, 10, 100));
    saved = c;
    CHECK(!tc_anchor(&c, 1000, EPOCH + 3000000, 10, 100));
    CHECK(memcmp(&c, &saved, sizeof(c)) == 0);
    CHECK(!tc_anchor(&c, 3000000, EPOCH + 2000050, 10, 100));
    CHECK(memcmp(&c, &saved, sizeof(c)) == 0);
    CHECK(!tc_anchor(&c, 3000000, EPOCH + 3010000, 10, 100));
    CHECK(memcmp(&c, &saved, sizeof(c)) == 0);
    CHECK(!tc_anchor(&c, 3000000, EPOCH + 3000100, UINT32_MAX, 1));
    CHECK(memcmp(&c, &saved, sizeof(c)) == 0);
    CHECK(tc_estimate(&c, 3000000, &utc, &error));
    CHECK(utc == EPOCH + 3000100);
    tc_init(&c);
    CHECK(tc_anchor(&c, 0, EPOCH, 0, 0));
    saved = c;
    CHECK(!tc_anchor(&c, 1000000, EPOCH + 1000700, 0, 0));
    CHECK(memcmp(&c, &saved, sizeof(c)) == 0);
    return 1;
}

static int overflow_and_large_epoch(void)
{
    tc_clock c, saved;
    uint64_t utc = 123, boot = UINT64_MAX - UINT64_C(100000000);
    uint32_t error = 456;
    tc_init(&c); saved = c;
    CHECK(!tc_anchor(&c, 0, UINT64_MAX, 0, 0));
    CHECK(memcmp(&c, &saved, sizeof(c)) == 0);
    CHECK(!tc_anchor(&c, UINT64_MAX, 1, 0, 0));
    CHECK(memcmp(&c, &saved, sizeof(c)) == 0);
    CHECK(tc_anchor(&c, boot, UINT64_MAX - 100, 0, 0));
    CHECK(tc_estimate(&c, boot + 100, &utc, &error));
    CHECK(utc == UINT64_MAX);
    utc = 123; error = 456;
    CHECK(!tc_estimate(&c, boot + 101, &utc, &error));
    CHECK(utc == 123 && error == 456);
    /* Integer epoch arithmetic retains an individual microsecond near 2^64. */
    CHECK(tc_estimate(&c, boot + 1, &utc, &error));
    CHECK(utc == UINT64_MAX - 99);
    return 1;
}

static double seconds(void)
{
#ifdef _WIN32
    LARGE_INTEGER tick, frequency;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&tick);
    return (double)tick.QuadPart / (double)frequency.QuadPart;
#else
    return (double)clock() / CLOCKS_PER_SEC;
#endif
}

static int benchmark(void)
{
    tc_clock c;
    uint32_t i, error;
    uint64_t utc;
    volatile uint64_t checksum = 0;
    double start, elapsed;
    tc_init(&c);
    start = seconds();
    for (i = 0; i < 100000; ++i) {
        uint64_t boot = (uint64_t)i * 1000000;
        if (!tc_anchor(&c, boot, EPOCH + boot + (uint64_t)i * 50, 1000, 1000) ||
            !tc_estimate(&c, boot + 500000, &utc, &error)) return 0;
        checksum += utc ^ error;
    }
    elapsed = seconds() - start;
    if (elapsed <= 0) return 0;
    printf("{\"anchor_and_estimate_pairs\":100000,\"seconds\":%.9g,"
           "\"ns_per_pair\":%.9g,\"checksum\":\"%" PRIu64 "\"}\n",
           elapsed, elapsed * 1e9 / 100000, checksum);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--bench")) return benchmark() ? 0 : 1;
    if (argc != 1) return 2;
    if (!no_anchor_and_single() || !known_drift_and_ring() ||
        !quantization_and_recovery() || !rejection_is_atomic() ||
        !overflow_and_large_epoch()) return 1;
    printf("PASS %u clock checks; sizeof_clock=%zu\n", checks, sizeof(tc_clock));
    return 0;
}
