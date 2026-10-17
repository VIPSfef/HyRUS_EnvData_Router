/* Independent numerical, boundary and serialization checks for telemetry.c. */
#include "telemetry.h"

#include <float.h>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

static unsigned checks;
#define CHECK(c) do { ++checks; if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 0; \
} } while (0)

static int close_enough(double got, double expected, double tolerance)
{
    return isfinite(got) && fabs(got - expected) <= tolerance;
}

static tb_sample sample(uint64_t t, uint32_t seq, float x)
{
    tb_sample s;
    unsigned k;
    memset(&s, 0, sizeof(s));
    s.t_us = t;
    s.seq = seq;
    s.quality = 5;
    s.p[0] = 123456789.12345679;
    s.p[1] = -987654321.01234567;
    s.p[2] = 120.125;
    s.v[0] = 35.5f;
    s.v[1] = -2.25f;
    s.v[2] = 0.125f;
    for (k = 0; k < TB_CHANNELS; ++k) s.value[k] = x;
    s.nav_age_us = 1234;
    return s;
}

static int check_known_statistics(void)
{
    tb_batch b;
    unsigned i, k;
    tb_batch_init(&b, 1000000);
    for (i = 0; i < 4; ++i) {
        tb_sample s = sample(1000000 + i * 100000, i + 1, (float)(i + 1));
        CHECK(tb_batch_add(&b, &s) == 1);
    }
    CHECK(b.n == 4 && b.raw_n == 4 && b.raw_dropped == 0);
    for (k = 0; k < TB_CHANNELS; ++k) {
        CHECK(b.stat[k].n == 4);
        CHECK(close_enough(b.stat[k].mean, 2.5, 1e-12));
        CHECK(close_enough(b.stat[k].m2, 5.0, 1e-12));
        CHECK(b.stat[k].min == 1 && b.stat[k].max == 4);
    }
    CHECK(b.raw[2].t_us == 1200000 && b.raw[2].seq == 3);
    CHECK(b.raw[2].p[0] == 123456789.12345679);
    return 1;
}

static int check_constant_missing_and_single(void)
{
    tb_batch b;
    unsigned i;
    tb_batch_init(&b, 0);
    CHECK(b.n == 0 && b.raw_n == 0);
    for (i = 0; i < 3; ++i) {
        tb_sample s = sample(i + 1, i + 1, 42.0f);
        s.value[1] = NAN;
        s.value[2] = i == 0 ? 7.0f : NAN;
        CHECK(tb_batch_add(&b, &s) == 1);
    }
    CHECK(b.stat[0].n == 3 && b.stat[0].mean == 42.0 && b.stat[0].m2 == 0);
    CHECK(b.stat[0].min == 42 && b.stat[0].max == 42);
    CHECK(b.stat[1].n == 0);
    CHECK(b.stat[2].n == 1 && b.stat[2].mean == 7 && b.stat[2].m2 == 0);
    return 1;
}

static int check_large_baseline(void)
{
    tb_batch b;
    unsigned i;
    tb_batch_init(&b, 0);
    for (i = 0; i < 4; ++i) {
        tb_sample s = sample(i, i, 10);
        s.value[0] = 100000000.0f + (float)i * 8.0f;
        CHECK(tb_batch_add(&b, &s) == 1);
    }
    CHECK(b.stat[0].mean == 100000012.0);
    CHECK(b.stat[0].m2 == 320.0);
    return 1;
}

static int check_rejection(void)
{
    tb_batch b;
    tb_sample s;
    unsigned k;
    tb_batch_init(&b, 1000000);
    s = sample(999999, 1, 10);
    CHECK(tb_batch_add(&b, &s) == 0);
    s.t_us = 2000000;
    CHECK(tb_batch_add(&b, &s) == 0);
    s.t_us = 1000000;
    CHECK(tb_batch_add(&b, &s) == 1);
    s.seq = 2;
    CHECK(tb_batch_add(&b, &s) == 0); /* duplicate time */
    s.t_us = 1000001; s.seq = 1;
    CHECK(tb_batch_add(&b, &s) == 0); /* duplicate sequence */
    s.t_us = 1000001; s.seq = 0;
    CHECK(tb_batch_add(&b, &s) == 0); /* sequence moved backwards */
    s.t_us = 1000001; s.seq = 2;
    for (k = 0; k < 3; ++k) {
        tb_sample bad = s;
        bad.p[k] = INFINITY;
        CHECK(tb_batch_add(&b, &bad) == 0);
        bad = s; bad.p[k] = NAN;
        CHECK(tb_batch_add(&b, &bad) == 0);
        bad = s; bad.p[k] = 1e9 + 1.0;
        CHECK(tb_batch_add(&b, &bad) == 0);
        bad = s; bad.v[k] = INFINITY;
        CHECK(tb_batch_add(&b, &bad) == 0);
        bad = s; bad.v[k] = NAN;
        CHECK(tb_batch_add(&b, &bad) == 0);
        bad = s; bad.v[k] = 2e9f;
        CHECK(tb_batch_add(&b, &bad) == 0);
    }
    for (k = 0; k < TB_CHANNELS; ++k) {
        tb_sample bad = s;
        bad.value[k] = INFINITY;
        CHECK(tb_batch_add(&b, &bad) == 0);
        bad.value[k] = -INFINITY;
        CHECK(tb_batch_add(&b, &bad) == 0);
        bad.value[k] = 2e9f;
        CHECK(tb_batch_add(&b, &bad) == 0);
    }
    {
        tb_sample bad = s;
        bad.value[0] = -274;
        CHECK(tb_batch_add(&b, &bad) == 0);
        bad = s; bad.value[1] = -0.01f;
        CHECK(tb_batch_add(&b, &bad) == 0);
        bad = s; bad.value[1] = 100.01f;
        CHECK(tb_batch_add(&b, &bad) == 0);
        bad = s; bad.value[2] = -0.01f;
        CHECK(tb_batch_add(&b, &bad) == 0);
        bad = s; bad.value[3] = -0.01f;
        CHECK(tb_batch_add(&b, &bad) == 0);
        bad = s; bad.nav_age_us = TB_MAX_NAV_AGE_US + 1;
        CHECK(tb_batch_add(&b, &bad) == 0);
    }
    CHECK(b.n == 1 && b.raw_n == 1 && b.stat[0].mean == 10);
    s.nav_age_us = TB_MAX_NAV_AGE_US;
    s.value[1] = 100;
    s.value[2] = 0;
    CHECK(tb_batch_add(&b, &s) == 1);
    s.t_us = 1000000; s.seq = 3;
    CHECK(tb_batch_add(&b, &s) == 0); /* time moved backwards */
    return 1;
}

static int check_sequence_and_window_edges(void)
{
    tb_batch b;
    tb_sample s;
    tb_batch_init(&b, UINT64_C(1800000000000000));
    s = sample(UINT64_C(1800000000000100), UINT32_MAX, 12);
    CHECK(tb_batch_add(&b, &s) == 1);
    s.t_us++; s.seq = 0;
    CHECK(tb_batch_add(&b, &s) == 0);
    tb_batch_init(&b, 0);
    s = sample(0, 0, 0);
    CHECK(tb_batch_add(&b, &s) == 1);
    s.t_us = TB_WINDOW_US - 1; s.seq = 1;
    CHECK(tb_batch_add(&b, &s) == 1);
    s.t_us = TB_WINDOW_US; s.seq = 2;
    CHECK(tb_batch_add(&b, &s) == 0);
    tb_batch_init(&b, UINT64_MAX / TB_WINDOW_US * TB_WINDOW_US);
    s = sample(UINT64_MAX, 1, 10);
    CHECK(tb_batch_add(&b, &s) == 1);
    return 1;
}

static int check_capacity_statistics(void)
{
    tb_batch b;
    unsigned i;
    double expected_mean;
    const unsigned count = TB_CAPACITY + 37;
    tb_batch_init(&b, 0);
    for (i = 0; i < count; ++i) {
        tb_sample s = sample(i + 1, i, (float)(i % 100));
        CHECK(tb_batch_add(&b, &s) == 1);
    }
    expected_mean = 0;
    for (i = 0; i < count; ++i) expected_mean += i % 100;
    expected_mean /= count;
    CHECK(b.n == count && b.raw_n == TB_CAPACITY && b.raw_dropped == 37);
    CHECK(b.stat[0].n == count);
    CHECK(close_enough(b.stat[0].mean, expected_mean, 1e-11));
    CHECK(b.raw[0].seq == 0 && b.raw[TB_CAPACITY - 1].seq == TB_CAPACITY - 1);
    return 1;
}

static int check_serializer_bounds(void)
{
    tb_batch b;
    tb_sample s = sample(12, 1, 10);
    char large[65536], exact[65536];
    struct { unsigned char prefix[16]; char out[32]; unsigned char suffix[16]; } guard;
    size_t cap, i;
    int n;
    tb_batch_init(&b, 0);
    CHECK(tb_batch_json(&b, large, sizeof(large)) > 0);
    CHECK(tb_batch_add(&b, &s) == 1);
    n = tb_batch_json(&b, large, sizeof(large));
    CHECK(n > 0 && (size_t)n == strlen(large));
    CHECK(tb_batch_json(&b, exact, (size_t)n + 1) == n);
    CHECK(strcmp(large, exact) == 0);
    CHECK(tb_batch_json(&b, exact, (size_t)n) < 0);
    for (cap = 0; cap <= sizeof(guard.out); ++cap) {
        memset(&guard, 0xA5, sizeof(guard));
        CHECK(tb_batch_json(&b, guard.out, cap) < 0);
        for (i = 0; i < sizeof(guard.prefix); ++i) CHECK(guard.prefix[i] == 0xA5);
        for (i = cap; i < sizeof(guard.out); ++i) CHECK((unsigned char)guard.out[i] == 0xA5);
        for (i = 0; i < sizeof(guard.suffix); ++i) CHECK(guard.suffix[i] == 0xA5);
    }
    return 1;
}

static int fixture(const char *kind)
{
    tb_batch b;
    char out[65536];
    unsigned i, n;
    const uint64_t epoch = strcmp(kind, "large_time") == 0 ?
        UINT64_MAX / TB_WINDOW_US * TB_WINDOW_US : UINT64_C(1800000000000000);
    tb_batch_init(&b, epoch);
    n = strcmp(kind, "empty") == 0 ? 0 : strcmp(kind, "overflow") == 0 ? TB_CAPACITY + 37 :
        strcmp(kind, "single") == 0 || strcmp(kind, "large_time") == 0 ? 1 : 4;
    for (i = 0; i < n; ++i) {
        tb_sample s = sample(strcmp(kind, "large_time") == 0 ? UINT64_MAX : epoch + i * 1000,
                             100 + i, (float)(i % 100));
        s.p[2] = 120.125 + i * 0.1;
        s.value[1] = i == 1 ? NAN : (float)(40 + i % 20);
        s.value[2] = i == 2 ? NAN : (float)(2.5 + (i % 7) * 0.25);
        s.value[3] = NAN;
        if (!tb_batch_add(&b, &s)) return 0;
    }
    if (tb_batch_json_mode(&b, out, sizeof(out), strcmp(kind, "summary") != 0) < 0) return 0;
    puts(out);
    return 1;
}

static double seconds(void)
{
#ifdef _WIN32
    LARGE_INTEGER now, hz;
    QueryPerformanceFrequency(&hz);
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)hz.QuadPart;
#else
    return (double)clock() / CLOCKS_PER_SEC;
#endif
}

static int benchmark(void)
{
    tb_batch b;
    tb_sample s = sample(0, 0, 10);
    uint32_t i;
    uint64_t window_us = 0;
    double start, elapsed;
    volatile uint32_t accepted = 0;
    start = seconds();
    for (i = 0; i < 1000000; ++i) {
        if (i % 100 == 0) {
            window_us = (uint64_t)(i / 100) * TB_WINDOW_US;
            tb_batch_init(&b, window_us);
        }
        s.t_us = window_us + (i % 100) * 10000; s.seq = i;
        s.value[0] = (float)(i % 101) * 0.5f;
        accepted += (uint32_t)tb_batch_add(&b, &s);
    }
    elapsed = seconds() - start;
    if (accepted != 1000000 || elapsed <= 0 || b.n != 100 ||
        b.raw_n != 100 || b.raw_dropped != 0) return 0;
    printf("{\"scenario\":\"normal_100_samples_per_window\",\"windows\":10000,"
           "\"samples_per_window\":100,\"initialization_included\":true,"
           "\"samples\":%" PRIu32 ",\"seconds\":%.9g,\"updates_per_second\":%.9g,"
           "\"ns_per_update\":%.9g,\"sizeof_sample\":%zu,\"sizeof_batch\":%zu}\n",
           accepted, elapsed, accepted / elapsed, elapsed * 1e9 / accepted,
           sizeof(tb_sample), sizeof(tb_batch));
    return 1;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--fixture") == 0) return fixture(argv[2]) ? 0 : 1;
    if (argc == 2 && strcmp(argv[1], "--bench") == 0) return benchmark() ? 0 : 1;
    if (argc != 1) return 2;
    if (!check_known_statistics() || !check_constant_missing_and_single() || !check_large_baseline() ||
        !check_rejection() || !check_sequence_and_window_edges() ||
        !check_capacity_statistics() || !check_serializer_bounds()) return 1;
    printf("PASS %u C checks\n", checks);
    return 0;
}
