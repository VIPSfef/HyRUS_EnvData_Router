#include "weather.h"

#include <float.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int uncertainty_valid(double x) { return isnan(x) || (isfinite(x) && x >= 0.0); }

int weather_sample_valid(const weather_sample *s) {
    unsigned i;
    if (!s || (s->metric != WEATHER_TEMPERATURE && s->metric != WEATHER_HUMIDITY) ||
        !isfinite(s->value) || s->value < 0.0 || s->value > FLT_MAX ||
        (s->metric == WEATHER_HUMIDITY && s->value > 1.0) ||
        s->has_time > 1 || s->has_position > 1 ||
        s->has_velocity > 1 || (s->has_velocity && !s->has_position) ||
        s->position_kind > WEATHER_POSITION_ESTGPS ||
        (!s->has_position && s->position_kind != WEATHER_POSITION_UNKNOWN) ||
        (s->has_position && !s->has_time) || !uncertainty_valid(s->measurement_variance)) return 0;
    if (s->has_position) {
        if (!uncertainty_valid(s->pos_error_m)) return 0;
        for (i = 0; i < 3; ++i) if (!isfinite(s->p[i]) || !isfinite(s->v[i])) return 0;
    }
    return 1;
}

void weather_batch_init(weather_batch *b, uint64_t host_window_ms) {
    if (!b) return;
    memset(b, 0, sizeof(*b));
    b->host_window_ms = host_window_ms;
}

static int sample_cmp(const weather_sample *a, const weather_sample *b) {
    unsigned i;
#define CMP_FIELD(field) do { if (a->field < b->field) return -1; if (a->field > b->field) return 1; } while (0)
    CMP_FIELD(metric); CMP_FIELD(message_id); CMP_FIELD(sensor_id); CMP_FIELD(sensor_instance);
    if (a->has_time != b->has_time) return a->has_time ? -1 : 1;
    if (a->has_time) { CMP_FIELD(t_us); }
    CMP_FIELD(seq); CMP_FIELD(value);
    if (a->has_position != b->has_position) return a->has_position ? -1 : 1;
    if (a->has_position) {
        for (i = 0; i < 3; ++i) {
            if (a->p[i] < b->p[i]) return -1;
            if (a->p[i] > b->p[i]) return 1;
        }
    }
    CMP_FIELD(position_kind); CMP_FIELD(estimation_age_us); CMP_FIELD(has_velocity); CMP_FIELD(navigation_segment);
    CMP_FIELD(quality); CMP_FIELD(time_uncert_us);
#undef CMP_FIELD
    return 0;
}

static double positioning_estimate(const weather_sample *s) {
    double speed;
    if (!s->has_time || !s->has_position || !isfinite(s->pos_error_m) ||
        s->time_uncert_us == UINT32_MAX) return NAN;
    speed = hypot(hypot((double)s->v[0], s->v[1]), s->v[2]);
    return (double)s->pos_error_m + speed * (double)s->time_uncert_us / 1e6;
}

static int representative_better(const weather_sample *a, const weather_sample *b) {
    double ea, eb;
    int ka = a->has_time && a->has_position, kb = b->has_time && b->has_position;
    if (ka != kb) return ka > kb;
    if (a->has_position && b->has_position && a->position_kind != b->position_kind) {
        unsigned pa=a->position_kind==WEATHER_POSITION_GPS ? 2u : a->position_kind==WEATHER_POSITION_ESTGPS ? 1u : 0u;
        unsigned pb=b->position_kind==WEATHER_POSITION_GPS ? 2u : b->position_kind==WEATHER_POSITION_ESTGPS ? 1u : 0u;
        return pa>pb;
    }
    if (a->has_time != b->has_time) return a->has_time > b->has_time;
    ea = positioning_estimate(a); eb = positioning_estimate(b);
    ka = isfinite(ea); kb = isfinite(eb);
    if (ka != kb) return ka > kb;
    if (ka && ea != eb) return ea < eb;
    ka = isfinite(a->measurement_variance); kb = isfinite(b->measurement_variance);
    if (ka != kb) return ka > kb;
    if (ka && a->measurement_variance != b->measurement_variance)
        return a->measurement_variance < b->measurement_variance;
    ka = a->has_time && a->time_uncert_us != UINT32_MAX;
    kb = b->has_time && b->time_uncert_us != UINT32_MAX;
    if (ka != kb) return ka > kb;
    if (ka && a->time_uncert_us != b->time_uncert_us) return a->time_uncert_us < b->time_uncert_us;
    return sample_cmp(a, b) < 0;
}

int weather_batch_add(weather_batch *b, const weather_sample *input) {
    weather_sample s;
    weather_group *g;
    unsigned i;
    double d;
    if (!b || !weather_sample_valid(input) || b->n == UINT32_MAX) return 0;
    for (i = 0; i < b->group_n; ++i) {
        g = &b->group[i];
        if (g->metric == input->metric && g->message_id == input->message_id && g->sensor_id == input->sensor_id &&
            g->sensor_instance == input->sensor_instance) break;
    }
    if (i == b->group_n && b->group_n == WEATHER_GROUP_CAPACITY) return 0;
    s = *input;
    s.quality &= ~(WEATHER_QUALITY_TIME_UNKNOWN | WEATHER_QUALITY_POSITION_UNKNOWN | WEATHER_QUALITY_POSITION_ESTIMATED);
    if (!s.has_position) s.estimation_age_us=UINT32_MAX;
    if (s.position_kind==WEATHER_POSITION_GPS) s.estimation_age_us=0;
    if (s.position_kind==WEATHER_POSITION_ESTGPS) s.quality |= WEATHER_QUALITY_POSITION_ESTIMATED;
    if (!s.has_time) s.quality |= WEATHER_QUALITY_TIME_UNKNOWN;
    if (!s.has_position) s.quality |= WEATHER_QUALITY_POSITION_UNKNOWN;
    g = &b->group[i];
    if (i == b->group_n) {
        memset(g, 0, sizeof(*g));
        g->metric = s.metric; g->message_id = s.message_id; g->sensor_id = s.sensor_id; g->sensor_instance = s.sensor_instance;
        g->min = g->max = s.value;
        g->representative = s;
        ++b->group_n;
    } else if (representative_better(&s, &g->representative)) g->representative = s;
    ++g->n; ++b->n;
    d = s.value - g->mean;
    g->mean += d / g->n;
    g->m2 += d * (s.value - g->mean);
    if (s.value < g->min) g->min = s.value;
    if (s.value > g->max) g->max = s.value;
    if (s.has_time) {
        if (!g->time_known_n || s.t_us < g->t_start_us) g->t_start_us = s.t_us;
        if (!g->time_known_n || s.t_us > g->t_end_us) g->t_end_us = s.t_us;
        ++g->time_known_n;
    }
    if (b->raw_n < WEATHER_CAPACITY) {
        b->raw[b->raw_n++] = s;
        ++g->raw_n;
    } else { ++b->raw_dropped; ++g->raw_dropped; }
    b->prepared = 0;
    return 1;
}

static int group_cmp(const weather_group *a, const weather_group *b) {
    if (a->metric != b->metric) return a->metric < b->metric ? -1 : 1;
    if (a->message_id != b->message_id) return a->message_id < b->message_id ? -1 : 1;
    if (a->sensor_id != b->sensor_id) return a->sensor_id < b->sensor_id ? -1 : 1;
    if (a->sensor_instance != b->sensor_instance) return a->sensor_instance < b->sensor_instance ? -1 : 1;
    return 0;
}

static void sample_sift(weather_sample *a, unsigned root, unsigned n) {
    weather_sample tmp;
    for (;;) {
        unsigned child = 2u * root + 1u;
        if (child >= n) return;
        if (child + 1u < n && sample_cmp(&a[child], &a[child + 1u]) < 0) ++child;
        if (sample_cmp(&a[root], &a[child]) >= 0) return;
        tmp = a[root]; a[root] = a[child]; a[child] = tmp; root = child;
    }
}

static void group_sift(weather_group *a, unsigned root, unsigned n) {
    weather_group tmp;
    for (;;) {
        unsigned child = 2u * root + 1u;
        if (child >= n) return;
        if (child + 1u < n && group_cmp(&a[child], &a[child + 1u]) < 0) ++child;
        if (group_cmp(&a[root], &a[child]) >= 0) return;
        tmp = a[root]; a[root] = a[child]; a[child] = tmp; root = child;
    }
}

void weather_batch_prepare(weather_batch *b) {
    unsigned i;
    weather_sample sample_tmp;
    weather_group group_tmp;
    if (!b || b->prepared) return;
    for (i = b->raw_n / 2u; i > 0; --i) sample_sift(b->raw, i - 1u, b->raw_n);
    for (i = b->raw_n; i > 1; --i) {
        sample_tmp = b->raw[0]; b->raw[0] = b->raw[i - 1u]; b->raw[i - 1u] = sample_tmp;
        sample_sift(b->raw, 0, i - 1u);
    }
    for (i = b->group_n / 2u; i > 0; --i) group_sift(b->group, i - 1u, b->group_n);
    for (i = b->group_n; i > 1; --i) {
        group_tmp = b->group[0]; b->group[0] = b->group[i - 1u]; b->group[i - 1u] = group_tmp;
        group_sift(b->group, 0, i - 1u);
    }
    b->prepared = 1;
}

typedef struct { char *buf; size_t pos, cap; int failed; } writer;
static void put(writer *w, const char *format, ...) {
    int n;
    va_list args;
    if (w->failed) return;
    va_start(args, format);
    n = vsnprintf(w->buf + w->pos, w->cap - w->pos, format, args);
    va_end(args);
    if (n < 0 || (size_t)n >= w->cap - w->pos) { w->failed = 1; return; }
    w->pos += (size_t)n;
}
static void number(writer *w, double x) {
    if (isfinite(x)) put(w, "%.17g", x); else put(w, "null");
}
static void time_value(writer *w, int known, uint64_t t) {
    if (known) put(w, "\"%" PRIu64 "\"", t); else put(w, "null");
}
static void sample_json(writer *w, const weather_sample *s) {
    put(w, "{\"t_us\":"); time_value(w, s->has_time, s->t_us);
    put(w, ",\"position_m\":");
    if (s->has_position) put(w, "[%.17g,%.17g,%.17g]", s->p[0], s->p[1], s->p[2]); else put(w, "null");
    put(w, ",\"velocity_mps\":");
    if (s->has_position) put(w, "[%.9g,%.9g,%.9g]", (double)s->v[0], (double)s->v[1], (double)s->v[2]); else put(w, "null");
    put(w, ",\"value\":%.17g,\"measurement_variance\":", s->value); number(w, s->measurement_variance);
    put(w, ",\"time_uncertainty_us\":");
    if (s->has_time && s->time_uncert_us != UINT32_MAX) put(w, "%" PRIu32, s->time_uncert_us); else put(w, "null");
    put(w, ",\"navigation_uncertainty_m\":"); number(w, s->has_position ? s->pos_error_m : NAN);
    put(w, ",\"position_uncertainty_m\":"); number(w, positioning_estimate(s));
    put(w, ",\"quality\":%" PRIu32 ",\"seq\":%" PRIu32, s->quality, s->seq);
    put(w, ",\"position_source\":\"%s\",\"estimation_age_us\":",
        s->has_position && s->position_kind==WEATHER_POSITION_GPS ? "GPS" :
        s->has_position && s->position_kind==WEATHER_POSITION_ESTGPS ? "ESTGPS" : "UNKNOWN");
    if(s->has_position && s->position_kind!=WEATHER_POSITION_UNKNOWN && s->estimation_age_us!=UINT32_MAX)
        put(w,"%" PRIu32,s->estimation_age_us);
    else put(w,"null");
    put(w,",\"velocity_valid\":%s,\"navigation_segment\":%" PRIu32,
        s->has_velocity ? "true" : "false",s->has_position ? s->navigation_segment : 0);
    put(w,"}");
}

int weather_sample_json(const weather_sample *s,char *buf,size_t cap) {
    writer w={buf,0,cap,0};
    weather_sample normalized;
    if(!buf || !cap) return -1;
    buf[0]='\0';
    if(!weather_sample_valid(s)) return -1;
    normalized=*s;
    normalized.quality&=~(WEATHER_QUALITY_TIME_UNKNOWN|WEATHER_QUALITY_POSITION_UNKNOWN|WEATHER_QUALITY_POSITION_ESTIMATED);
    if(!s->has_time) normalized.quality|=WEATHER_QUALITY_TIME_UNKNOWN;
    if(!s->has_position) { normalized.quality|=WEATHER_QUALITY_POSITION_UNKNOWN; normalized.estimation_age_us=UINT32_MAX; }
    if(s->position_kind==WEATHER_POSITION_GPS) normalized.estimation_age_us=0;
    if(s->position_kind==WEATHER_POSITION_ESTGPS) normalized.quality|=WEATHER_QUALITY_POSITION_ESTIMATED;
    sample_json(&w,&normalized);
    if(w.failed || w.pos>INT_MAX) { buf[0]='\0'; return -1; }
    return (int)w.pos;
}
int weather_batch_json(const weather_batch *b, char *buf, size_t cap) {
    writer w;
    unsigned metric, i, j;
    if (buf && cap) buf[0] = '\0';
    if (!b || !buf || !cap || !b->prepared || b->group_n > WEATHER_GROUP_CAPACITY ||
        b->raw_n > WEATHER_CAPACITY) return -1;
    w.buf = buf; w.pos = 0; w.cap = cap; w.failed = 0;
    put(&w, "{\"si_registry\":%u,\"host_window_ms\":\"%" PRIu64 "\",\"n\":%" PRIu32
        ",\"raw_n\":%" PRIu32 ",\"raw_dropped\":%" PRIu32 ",\"measurements\":{",
        WEATHER_SI_REGISTRY, b->host_window_ms, b->n, b->raw_n, b->raw_dropped);
    for (metric = WEATHER_TEMPERATURE; metric <= WEATHER_HUMIDITY; ++metric) {
        int comma = 0;
        put(&w, "%s\"%s\":[", metric == WEATHER_TEMPERATURE ? "" : ",",
            metric == WEATHER_TEMPERATURE ? "temperature" : "humidity");
        for (i = 0; i < b->group_n; ++i) {
            const weather_group *g = &b->group[i];
            int raw_comma = 0;
            if (g->metric != metric) continue;
            put(&w, "%s{\"message_id\":%" PRIu32 ",\"sensor_id\":%" PRIu32 ",\"sensor_instance\":%u,\"representative\":",
                comma ? "," : "", g->message_id, g->sensor_id, (unsigned)g->sensor_instance);
            comma = 1;
            sample_json(&w, &g->representative);
            put(&w, ",\"statistics\":{\"n\":%" PRIu32 ",\"mean\":%.17g,\"variance\":", g->n, g->mean);
            number(&w, g->n > 1 ? fmax(0.0, g->m2 / (g->n - 1u)) : NAN);
            put(&w, ",\"min\":%.17g,\"max\":%.17g,\"time_known_n\":%" PRIu32 ",\"t_start_us\":",
                g->min, g->max, g->time_known_n);
            time_value(&w, g->time_known_n != 0, g->t_start_us);
            put(&w, ",\"t_end_us\":"); time_value(&w, g->time_known_n != 0, g->t_end_us);
            put(&w, "},\"raw_n\":%" PRIu32 ",\"raw_dropped\":%" PRIu32 ",\"samples\":[", g->raw_n, g->raw_dropped);
            for (j = 0; j < b->raw_n; ++j) {
                const weather_sample *s = &b->raw[j];
                if (s->metric != g->metric || s->message_id != g->message_id || s->sensor_id != g->sensor_id || s->sensor_instance != g->sensor_instance) continue;
                put(&w, "%s", raw_comma ? "," : ""); raw_comma = 1;
                sample_json(&w, s);
            }
            put(&w, "]}");
        }
        put(&w, "]");
    }
    put(&w, "}}");
    if (w.failed || w.pos > INT_MAX) { buf[0] = '\0'; return -1; }
    return (int)w.pos;
}
