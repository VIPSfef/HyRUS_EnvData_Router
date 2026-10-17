#include "telemetry.h"

#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

int tb_sample_valid(const tb_sample *s)
{
    unsigned i;
    if (!s || s->nav_age_us > TB_MAX_NAV_AGE_US) return 0;
    for (i = 0; i < 3; ++i)
        if (!isfinite(s->p[i]) || fabs(s->p[i]) > 1e9 ||
            !isfinite(s->v[i]) || fabs((double)s->v[i]) > 1e9) return 0;
    for (i = 0; i < TB_CHANNELS; ++i)
        if (!isnan(s->value[i]) &&
            (!isfinite(s->value[i]) || fabs((double)s->value[i]) > 1e9))
            return 0;
    if (s->value[0] < -273.15f || s->value[1] < 0 ||
        s->value[1] > 100 || s->value[2] < 0 || s->value[3] < 0) return 0;
    return 1;
}

void tb_batch_init(tb_batch *b, uint64_t window_us)
{
    if (!b) return;
    memset(b, 0, sizeof(*b));
    b->window_us = window_us;
}

static void stat_add(tb_stat *st, double x)
{
    double delta;
    if (!st->n) {
        st->n = 1;
        st->mean = st->min = st->max = x;
        return;
    }
    ++st->n;
    delta = x - st->mean;
    st->mean += delta / st->n;
    st->m2 += delta * (x - st->mean);
    if (x < st->min) st->min = x;
    if (x > st->max) st->max = x;
}

int tb_batch_add(tb_batch *b, const tb_sample *s)
{
    unsigned i;
    if (!b || !tb_sample_valid(s) || b->n == UINT32_MAX ||
        s->t_us / TB_WINDOW_US * TB_WINDOW_US != b->window_us ||
        (b->n && (s->t_us <= b->last_t_us || s->seq <= b->last_seq)))
        return 0;
    ++b->n;
    b->last_t_us = s->t_us;
    b->last_seq = s->seq;
    for (i = 0; i < TB_CHANNELS; ++i)
        if (!isnan(s->value[i])) stat_add(&b->stat[i], s->value[i]);
    if (b->raw_n < TB_CAPACITY) b->raw[b->raw_n++] = *s;
    else ++b->raw_dropped;
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
    if (b->raw_n > TB_CAPACITY || b->raw_n > b->n) return -1;
    w.buf = buf; w.cap = cap; w.used = 0; w.failed = 0;
    put(&w, "{\"window_us\":\"%" PRIu64 "\",\"n\":%" PRIu32
        ",\"raw_n\":%" PRIu32 ",\"raw_dropped\":%" PRIu32
        ",\"raw_included\":%s,\"stats\":[",
        b->window_us, b->n, b->raw_n, b->raw_dropped,
        include_raw ? "true" : "false");
    for (i = 0; i < TB_CHANNELS; ++i) {
        const tb_stat *st = &b->stat[i];
        put(&w, "%s{\"n\":%" PRIu32 ",\"mean\":", i ? "," : "", st->n);
        if (!st->n) put(&w, "null,\"variance\":null,\"min\":null,\"max\":null}");
        else {
            put(&w, "%.17g,\"variance\":", st->mean);
            if (st->n < 2) put(&w, "null");
            else put(&w, "%.17g", st->m2 / (st->n - 1));
            put(&w, ",\"min\":%.17g,\"max\":%.17g}", st->min, st->max);
        }
    }
    put(&w, "],\"raw\":[");
    for (j = 0; include_raw && j < b->raw_n && !w.failed; ++j) {
        const tb_sample *s = &b->raw[j];
        put(&w, "%s[\"%" PRIu64 "\",%" PRIu32 ",%" PRIu32,
            j ? "," : "", s->t_us, s->seq, s->quality);
        for (i = 0; i < 3; ++i) put(&w, ",%.17g", s->p[i]);
        for (i = 0; i < 3; ++i) put(&w, ",%.9g", (double)s->v[i]);
        for (i = 0; i < TB_CHANNELS; ++i) {
            if (isnan(s->value[i])) put(&w, ",null");
            else put(&w, ",%.9g", (double)s->value[i]);
        }
        put(&w, ",%" PRIu32 "]", s->nav_age_us);
    }
    put(&w, "]}");
    return w.failed ? -1 : (int)w.used;
}

int tb_batch_json(const tb_batch *b, char *buf, size_t cap)
{
    return tb_batch_json_mode(b, buf, cap, 1);
}
