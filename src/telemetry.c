#include "telemetry.h"

#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int alpha_char(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static int text_valid(const char *s, size_t size, int unit)
{
    size_t i;
    if (!unit && !alpha_char((unsigned char)s[0]) && s[0] != '_') return 0;
    for (i = 0; i < size && s[i]; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (!alpha_char(c) && !(c >= '0' && c <= '9') && c != '_' &&
            c != '.' && c != '-' && !(unit && strchr("/%*^+", c))) return 0;
    }
    return i > 0 && i < size;
}

static int nav_valid(const double p[3], const float v[3], float error)
{
    unsigned i;
    if (!isfinite(error) || error < 0 || error > 1e9f) return 0;
    for (i = 0; i < 3; ++i)
        if (!isfinite(p[i]) || fabs(p[i]) > 1e9 ||
            !isfinite(v[i]) || fabs((double)v[i]) > 1e9) return 0;
    return 1;
}

int tb_sample_valid(const tb_sample *s)
{
    return s && text_valid(s->key, sizeof(s->key), 0) &&
        text_valid(s->unit, sizeof(s->unit), 1) &&
        (isnan(s->value) || (isfinite(s->value) && fabs(s->value) <= 1e9)) &&
        nav_valid(s->p, s->v, s->pos_error_m) &&
        s->nav_age_us <= TB_MAX_NAV_AGE_US &&
        s->time_uncert_us <= TB_MAX_TIME_UNCERT_US;
}

void tb_batch_init(tb_batch *b, uint64_t window_us)
{
    if (!b) return;
    memset(b, 0, sizeof(*b));
    b->window_us = window_us;
    b->prepared = 1;
}

static void stat_add(tb_stat *st, const tb_sample *s)
{
    double x = s->value, delta;
    double speed = sqrt((double)s->v[0] * s->v[0] +
                        (double)s->v[1] * s->v[1] +
                        (double)s->v[2] * s->v[2]);
    double error = s->pos_error_m + speed * s->time_uncert_us / 1e6;
    if (!st->n) {
        st->n = 1;
        st->mean = st->min = st->max = x;
        st->mean_time_uncert_us = st->max_time_uncert_us = s->time_uncert_us;
        st->mean_position_error_m = st->max_position_error_m = error;
        return;
    }
    ++st->n;
    delta = x - st->mean;
    st->mean += delta / st->n;
    st->m2 += delta * (x - st->mean);
    if (x < st->min) st->min = x;
    if (x > st->max) st->max = x;
    st->mean_time_uncert_us += (s->time_uncert_us - st->mean_time_uncert_us) / st->n;
    st->mean_position_error_m += (error - st->mean_position_error_m) / st->n;
    if (s->time_uncert_us > st->max_time_uncert_us)
        st->max_time_uncert_us = s->time_uncert_us;
    if (error > st->max_position_error_m) st->max_position_error_m = error;
}

int tb_batch_add(tb_batch *b, const tb_sample *s)
{
    uint32_t k;
    tb_stat *st;
    if (!b || !tb_sample_valid(s) || b->n == UINT32_MAX ||
        s->t_us / TB_WINDOW_US * TB_WINDOW_US != b->window_us) return 0;
    for (k = 0; k < b->key_n && strcmp(b->stat[k].key, s->key) < 0; ++k) {}
    if (k < b->key_n && !strcmp(b->stat[k].key, s->key)) {
        st = &b->stat[k];
        if (strcmp(st->unit, s->unit) || s->t_us <= st->last_t_us ||
            s->seq <= st->last_seq) return 0;
    } else {
        if (b->key_n == TB_MAX_KEYS) return 0;
        memmove(&b->stat[k + 1], &b->stat[k],
                (b->key_n - k) * sizeof(b->stat[0]));
        st = &b->stat[k];
        memset(st, 0, sizeof(*st));
        strcpy(st->key, s->key);
        strcpy(st->unit, s->unit);
        ++b->key_n;
    }
    ++b->n;
    st->last_t_us = s->t_us;
    st->last_seq = s->seq;
    if (!isnan(s->value)) stat_add(st, s);
    if (b->raw_n < TB_CAPACITY) b->raw[b->raw_n++] = *s;
    else ++b->raw_dropped;
    b->prepared = 0;
    return 1;
}

static int sample_less(const tb_sample *a, const tb_sample *b)
{
    int key_order;
    if (a->t_us != b->t_us) return a->t_us < b->t_us;
    key_order = strcmp(a->key, b->key);
    return key_order ? key_order < 0 : a->seq < b->seq;
}

static void swap_sample(tb_sample *a, tb_sample *b)
{
    tb_sample tmp = *a;
    *a = *b;
    *b = tmp;
}

static void sift(tb_sample *raw, uint32_t root, uint32_t n)
{
    while (root < n / 2) {
        uint32_t child = 2 * root + 1;
        if (child + 1 < n && sample_less(&raw[child], &raw[child + 1])) ++child;
        if (!sample_less(&raw[root], &raw[child])) return;
        swap_sample(&raw[root], &raw[child]);
        root = child;
    }
}

void tb_batch_prepare(tb_batch *b)
{
    uint32_t n, start;
    if (!b || b->prepared) return;
    n = b->raw_n;
    start = n / 2;
    while (start) sift(b->raw, --start, n);
    while (n > 1) {
        swap_sample(&b->raw[0], &b->raw[--n]);
        sift(b->raw, 0, n);
    }
    b->prepared = 1;
}

int tb_align(const tb_nav *a, const tb_nav *b, uint64_t t,
             double accel_bound, tb_sample *out)
{
    uint64_t span, left, right;
    double alpha, dt, error, p[3];
    float v[3], error_f;
    unsigned i;
    if (!a || !b || !out || !isfinite(accel_bound) || accel_bound < 0 ||
        accel_bound > 1e6 || !nav_valid(a->p, a->v, a->pos_error_m) ||
        !nav_valid(b->p, b->v, b->pos_error_m) || b->t_us < a->t_us ||
        t < a->t_us || t > b->t_us) return 0;
    span = b->t_us - a->t_us;
    if (span > TB_MAX_NAV_AGE_US) return 0;
    left = t - a->t_us;
    right = b->t_us - t;
    alpha = span ? (double)left / (double)span : 0;
    dt = (double)span / 1e6;
    error = (1 - alpha) * a->pos_error_m + alpha * b->pos_error_m +
            accel_bound * dt * dt / 8;
    if (error > 1e9) return 0;
    error_f = (float)error;
    if ((double)error_f < error) error_f = nextafterf(error_f, INFINITY);
    for (i = 0; i < 3; ++i) {
        p[i] = a->p[i] + alpha * (b->p[i] - a->p[i]);
        v[i] = (float)((double)a->v[i] + alpha * ((double)b->v[i] - a->v[i]));
    }
    out->t_us = t;
    memcpy(out->p, p, sizeof(p));
    memcpy(out->v, v, sizeof(v));
    out->nav_age_us = (uint32_t)(left > right ? left : right);
    out->pos_error_m = error_f;
    return 1;
}

typedef struct {
    char *buf;
    size_t cap, used;
    int failed;
} json_writer;

static void put(json_writer *w, const char *fmt, ...)
{
    int n;
    va_list ap;
    if (w->failed) return;
    va_start(ap, fmt);
    n = vsnprintf(w->buf + w->used, w->cap - w->used, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= w->cap - w->used ||
        (size_t)n > (size_t)INT_MAX - w->used) {
        w->failed = 1;
        return;
    }
    w->used += (size_t)n;
}

int tb_batch_json_mode(const tb_batch *b, char *buf, size_t cap, int include_raw)
{
    json_writer w;
    uint32_t j;
    unsigned i;
    if (!b || !buf || !cap) return -1;
    buf[0] = '\0';
    if (!b->prepared || b->raw_n > TB_CAPACITY || b->raw_n > b->n ||
        b->key_n > TB_MAX_KEYS) return -1;
    w.buf = buf; w.cap = cap; w.used = 0; w.failed = 0;
    put(&w, "{\"window_us\":\"%" PRIu64 "\",\"n\":%" PRIu32
        ",\"raw_n\":%" PRIu32 ",\"raw_dropped\":%" PRIu32
        ",\"key_n\":%" PRIu32 ",\"raw_included\":%s,\"stats\":[",
        b->window_us, b->n, b->raw_n, b->raw_dropped, b->key_n,
        include_raw ? "true" : "false");
    for (j = 0; j < b->key_n; ++j) {
        const tb_stat *st = &b->stat[j];
        put(&w, "%s{\"key\":\"%s\",\"unit\":\"%s\",\"n\":%" PRIu32 ",\"mean\":",
            j ? "," : "", st->key, st->unit, st->n);
        if (!st->n)
            put(&w, "null,\"variance\":null,\"min\":null,\"max\":null,"
                "\"time_uncert_mean_us\":null,\"time_uncert_max_us\":null,"
                "\"position_error_mean_m\":null,\"position_error_max_m\":null}");
        else {
            put(&w, "%.17g,\"variance\":", st->mean);
            if (st->n < 2) put(&w, "null");
            else put(&w, "%.17g", st->m2 / (st->n - 1));
            put(&w, ",\"min\":%.17g,\"max\":%.17g,"
                "\"time_uncert_mean_us\":%.17g,\"time_uncert_max_us\":%.17g,"
                "\"position_error_mean_m\":%.17g,\"position_error_max_m\":%.17g}",
                st->min, st->max, st->mean_time_uncert_us, st->max_time_uncert_us,
                st->mean_position_error_m, st->max_position_error_m);
        }
    }
    put(&w, "],\"raw\":[");
    for (j = 0; include_raw && j < b->raw_n && !w.failed; ++j) {
        const tb_sample *s = &b->raw[j];
        put(&w, "%s[\"%" PRIu64 "\",%" PRIu32 ",%" PRIu32 ",\"%s\",\"%s\"",
            j ? "," : "", s->t_us, s->seq, s->quality, s->key, s->unit);
        for (i = 0; i < 3; ++i) put(&w, ",%.17g", s->p[i]);
        for (i = 0; i < 3; ++i) put(&w, ",%.9g", (double)s->v[i]);
        if (isnan(s->value)) put(&w, ",null");
        else put(&w, ",%.17g", s->value);
        put(&w, ",%" PRIu32 ",%" PRIu32 ",%.9g,", s->time_uncert_us,
            s->nav_age_us, (double)s->pos_error_m);
        if (s->utc_us) put(&w, "\"%" PRIu64 "\",%" PRIu32 "]",
                           s->utc_us, s->utc_uncert_us);
        else put(&w, "null,null]");
    }
    put(&w, "]}");
    return w.failed ? -1 : (int)w.used;
}

int tb_batch_json(const tb_batch *b, char *buf, size_t cap)
{
    return tb_batch_json_mode(b, buf, cap, 1);
}
