/* Numerical oracle, unknown-time preservation, grouping and bounded storage. */
#include "weather.h"
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static unsigned checks;
static char json[200000];
#define CHECK(c) do { ++checks; if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 0; } } while (0)

static weather_sample sample(unsigned metric, uint32_t sensor, uint32_t seq, double value) {
    weather_sample s;
    memset(&s, 0, sizeof(s));
    s.metric = (uint8_t)metric; s.sensor_id = sensor; s.seq = seq; s.value = value;
    s.t_us = UINT64_C(1000000) + seq * UINT64_C(10000);
    s.has_time = s.has_position = 1;
    s.p[0] = (double)seq * 10.0; s.p[1] = -(double)seq; s.p[2] = 100.0;
    s.v[0] = 10.0f; s.pos_error_m = 0.5f; s.time_uncert_us = 100;
    s.measurement_variance = NAN;
    return s;
}
static const weather_group *find(const weather_batch *b, unsigned metric, uint32_t sensor, uint8_t instance) {
    unsigned i;
    for (i = 0; i < b->group_n; ++i) if (b->group[i].metric == metric &&
        b->group[i].sensor_id == sensor && b->group[i].sensor_instance == instance) return &b->group[i];
    return NULL;
}
static int unchanged(weather_batch *b, const weather_sample *s) {
    weather_batch before = *b;
    return !weather_batch_add(b, s) && !memcmp(b, &before, sizeof(*b));
}
static int grouping(void) {
    weather_batch b;
    weather_sample s;
    const weather_group *g;
    unsigned i;
    weather_batch_init(&b, 6000);
    for (i = 0; i < 4; ++i) {
        s = sample(WEATHER_TEMPERATURE, 7, i + 1, 293.15 + i);
        CHECK(weather_batch_add(&b, &s));
    }
    s = sample(WEATHER_HUMIDITY, 7, 5, 0.5); CHECK(weather_batch_add(&b, &s));
    s = sample(WEATHER_TEMPERATURE, 8, 6, 301.15); CHECK(weather_batch_add(&b, &s));
    s.sensor_id = 7; s.sensor_instance = 1; CHECK(weather_batch_add(&b, &s));
    CHECK(b.n == 7 && b.group_n == 4 && b.raw_n == 7 && !b.raw_dropped);
    g = find(&b, WEATHER_TEMPERATURE, 7, 0);
    CHECK(g && g->n == 4 && fabs(g->mean - 294.65) < 1e-10);
    CHECK(fabs(g->m2 - 5.0) < 1e-10 && g->time_known_n == 4);
    CHECK(g->t_start_us == 1010000 && g->t_end_us == 1040000);
    CHECK(g->representative.seq == 1 && g->representative.value != g->mean);
    CHECK(find(&b, WEATHER_TEMPERATURE, 8, 0)->n == 1);
    CHECK(find(&b, WEATHER_TEMPERATURE, 7, 1)->n == 1);
    CHECK(find(&b, WEATHER_HUMIDITY, 7, 0)->mean == 0.5);
    weather_batch_prepare(&b);
    CHECK(b.group[0].metric == WEATHER_TEMPERATURE && b.group[0].sensor_id == 7 && b.group[0].sensor_instance == 0);
    CHECK(b.group[1].sensor_id == 7 && b.group[1].sensor_instance == 1);
    CHECK(b.group[2].sensor_id == 8 && b.group[3].metric == WEATHER_HUMIDITY);
    CHECK(weather_batch_json(&b, json, sizeof(json)) > 0);
    CHECK(strstr(json, "\"si_registry\":1") && strstr(json, "\"temperature\":[") && strstr(json, "\"humidity\":["));
    CHECK(strstr(json, "\"variance\":null") && !strstr(json, "unit"));
    return 1;
}
static int protocol_identity(void) {
    weather_batch a, b;
    weather_sample s[8];
    unsigned i, gindex;
    const uint32_t ids[2] = {8009,12920};
    weather_batch_init(&a, 9000); weather_batch_init(&b, 9000);
    for (i=0;i<8;++i) {
        unsigned protocol=(i/2u)%2u, metric=i/4u;
        s[i]=sample(metric ? WEATHER_HUMIDITY : WEATHER_TEMPERATURE,0,i+1,
            metric ? (protocol ? 0.70 : 0.30)+(double)(i%2u)*0.02 : (protocol ? 305.0 : 295.0)+(double)(i%2u)*2.0);
        s[i].message_id=ids[protocol];
        if(protocol) s[i].has_time=s[i].has_position=0;
        CHECK(weather_batch_add(&a,&s[i]));
    }
    for(i=8;i>0;--i) CHECK(weather_batch_add(&b,&s[i-1]));
    CHECK(a.group_n==4 && a.n==8 && a.raw_n==8);
    weather_batch_prepare(&a); weather_batch_prepare(&b);
    CHECK(a.group[0].metric==WEATHER_TEMPERATURE && a.group[0].message_id==8009);
    CHECK(a.group[1].metric==WEATHER_TEMPERATURE && a.group[1].message_id==12920);
    CHECK(a.group[2].metric==WEATHER_HUMIDITY && a.group[2].message_id==8009);
    CHECK(a.group[3].metric==WEATHER_HUMIDITY && a.group[3].message_id==12920);
    CHECK(a.group[0].mean==296.0 && a.group[1].mean==306.0);
    CHECK(fabs(a.group[2].mean-0.31)<1e-12 && fabs(a.group[3].mean-0.71)<1e-12);
    for(gindex=0;gindex<4;++gindex) {
        const weather_group *g=&a.group[gindex];
        unsigned matching=0;
        CHECK(g->n==2 && g->raw_n==2 && !g->raw_dropped);
        CHECK(g->representative.message_id==g->message_id);
        CHECK(g->representative.value==b.group[gindex].representative.value);
        CHECK(g->time_known_n==(g->message_id==8009 ? 2u : 0u));
        for(i=0;i<a.raw_n;++i) if(a.raw[i].metric==g->metric && a.raw[i].message_id==g->message_id) ++matching;
        CHECK(matching==2);
    }
    for(i=0;i<8;++i) CHECK(a.raw[i].seq==b.raw[i].seq && a.raw[i].message_id==b.raw[i].message_id);
    CHECK(weather_batch_json(&a,json,sizeof(json))>0);
    { const char *at=json; unsigned count=0;
      while((at=strstr(at,"\"message_id\":"))!=NULL) { ++count; at+=13; }
      CHECK(count==4); /* group metadata is emitted once, not repeated per raw sample */
    }
    CHECK(strstr(json,"\"message_id\":8009,\"sensor_id\":0,\"sensor_instance\":0"));
    CHECK(strstr(json,"\"message_id\":12920,\"sensor_id\":0,\"sensor_instance\":0"));
    return 1;
}
static int missing_time(void) {
    weather_batch b;
    weather_sample s = sample(WEATHER_TEMPERATURE, 3, 1, 300.0);
    const weather_group *g;
    s.has_time = s.has_position = 0; /* stale nonzero metadata MUST be ignored */
    s.p[0] = NAN; s.pos_error_m = NAN; s.time_uncert_us = 1;
    weather_batch_init(&b, 123456);
    CHECK(weather_batch_add(&b, &s));
    g = find(&b, WEATHER_TEMPERATURE, 3, 0);
    CHECK(g && !g->time_known_n && g->n == 1 && g->mean == 300.0);
    CHECK(g->representative.quality & WEATHER_QUALITY_TIME_UNKNOWN);
    CHECK(g->representative.quality & WEATHER_QUALITY_POSITION_UNKNOWN);
    weather_batch_prepare(&b);
    CHECK(weather_batch_json(&b, json, sizeof(json)) > 0);
    CHECK(strstr(json, "\"t_us\":null,\"position_m\":null,\"velocity_mps\":null"));
    CHECK(strstr(json, "\"t_start_us\":null,\"t_end_us\":null"));
    CHECK(strstr(json, "\"time_uncertainty_us\":null"));
    CHECK(strstr(json, "\"measurement_variance\":null"));
    CHECK(strstr(json, "\"position_uncertainty_m\":null"));
    CHECK(strstr(json, "\"host_window_ms\":\"123456\""));
    s.has_time = 1; s.has_position = 0; s.t_us = 500;
    CHECK(weather_batch_add(&b, &s));
    g = find(&b, WEATHER_TEMPERATURE, 3, 0);
    CHECK(g->n == 2 && g->time_known_n == 1 && g->t_start_us == 500 && g->t_end_us == 500);
    CHECK(g->representative.has_time && !g->representative.has_position);
    CHECK(!(g->representative.quality & WEATHER_QUALITY_TIME_UNKNOWN));
    return 1;
}
static int representative(void) {
    weather_batch a, b;
    weather_sample s[5];
    unsigned i;
    for (i = 0; i < 5; ++i) s[i] = sample(WEATHER_TEMPERATURE, 4, i + 1, 290.0 + i);
    s[0].v[0] = 1000; s[0].time_uncert_us = 1000; s[0].pos_error_m = 1; /* estimate 2m */
    s[1].v[0] = 0; s[1].time_uncert_us = 10000; s[1].pos_error_m = 1; /* 1m despite worse clock */
    s[2] = s[1]; s[2].seq = 3; s[2].value = 292; s[2].measurement_variance = 0.25;
    s[3] = s[2]; s[3].seq = 4; s[3].value = 293; s[3].measurement_variance = 0.0625;
    s[4].has_position = 0; s[4].time_uncert_us = 1;
    weather_batch_init(&a, 0); weather_batch_init(&b, 0);
    for (i = 0; i < 5; ++i) CHECK(weather_batch_add(&a, &s[i]));
    for (i = 5; i > 0; --i) CHECK(weather_batch_add(&b, &s[i - 1]));
    CHECK(a.group[0].representative.seq == 4 && b.group[0].representative.seq == 4);
    CHECK(a.group[0].representative.value == 293 && a.group[0].mean == 292);
    CHECK(a.group[0].representative.p[0] == s[3].p[0]);
    weather_batch_prepare(&a); weather_batch_prepare(&b);
    for (i = 0; i < 5; ++i) CHECK(a.raw[i].seq == b.raw[i].seq);
    CHECK(weather_batch_json(&a, json, sizeof(json)) > 0);
    CHECK(strstr(json, "\"position_uncertainty_m\":1,"));
    CHECK(strstr(json, "\"measurement_variance\":0.0625"));
    return 1;
}
static int storage_and_numerics(void) {
    weather_batch b;
    weather_sample s;
    const weather_group *g;
    unsigned i;
    long double mean = 0, m2 = 0;
    weather_batch_init(&b, 0);
    for (i = 0; i < 1024; ++i) {
        s = sample(WEATHER_TEMPERATURE, 1, i, 100000000.0 + (double)(i % 13u) * 0.125);
        mean += (long double)s.value;
        CHECK(weather_batch_add(&b, &s));
    }
    mean /= 1024;
    for (i = 0; i < 1024; ++i) {
        long double d = (long double)(100000000.0 + (double)(i % 13u) * 0.125) - mean;
        m2 += d * d;
    }
    g = find(&b, WEATHER_TEMPERATURE, 1, 0);
    CHECK(g && g->n == 1024 && b.n == 1024);
    CHECK(b.raw_n == WEATHER_CAPACITY && b.raw_dropped == 1024u - WEATHER_CAPACITY);
    CHECK(g->raw_n == WEATHER_CAPACITY && g->raw_dropped == b.raw_dropped);
    CHECK(fabs(g->mean - (double)mean) < 2e-7);
    CHECK(fabs(g->m2 / 1023.0 - (double)(m2 / 1023.0L)) < 2e-8);
    CHECK(g->min == 100000000.0 && g->max == 100000001.5);
    weather_batch_prepare(&b);
    CHECK(weather_batch_json(&b, json, sizeof(json)) > 0);
    CHECK(strlen(json) < sizeof(json) && strstr(json, "\"n\":1024") && strstr(json, "\"raw_dropped\":896"));
    return 1;
}
static int limits(void) {
    weather_batch b;
    weather_sample s = sample(WEATHER_HUMIDITY, 1, 1, 0.4);
    unsigned i;
    weather_batch_init(&b, 0);
    s.value = NAN; CHECK(unchanged(&b, &s));
    s.value = INFINITY; CHECK(unchanged(&b, &s));
    s.value = -0.01; CHECK(unchanged(&b, &s));
    s.value = 1.01; CHECK(unchanged(&b, &s));
    s.value = 0; CHECK(weather_sample_valid(&s));
    s.value = 1; CHECK(weather_sample_valid(&s));
    s.has_time = 0; CHECK(unchanged(&b, &s));
    s.has_time = 1; s.measurement_variance = -1; CHECK(unchanged(&b, &s));
    s.measurement_variance = INFINITY; CHECK(unchanged(&b, &s));
    s.measurement_variance = NAN; s.pos_error_m = -1; CHECK(unchanged(&b, &s));
    s.pos_error_m = NAN; CHECK(weather_sample_valid(&s));
    s.v[1] = INFINITY; CHECK(unchanged(&b, &s)); s.v[1] = 0;
    s.p[2] = NAN; CHECK(unchanged(&b, &s)); s.p[2] = 0;
    s.metric = 3; CHECK(unchanged(&b, &s)); s.metric = WEATHER_HUMIDITY;
    s.has_time = 2; CHECK(unchanged(&b, &s)); s.has_time = 1;
    s.has_position = 2; CHECK(unchanged(&b, &s)); s.has_position = 1;
    for (i = 0; i < WEATHER_GROUP_CAPACITY; ++i) { s.sensor_id = i; CHECK(weather_batch_add(&b, &s)); }
    s.sensor_id = WEATHER_GROUP_CAPACITY; CHECK(unchanged(&b, &s));
    s.sensor_id = 0; CHECK(weather_batch_add(&b, &s));
    b.n = UINT32_MAX; CHECK(unchanged(&b, &s));
    CHECK(!weather_sample_valid(NULL)); CHECK(!weather_batch_add(NULL, &s));
    return 1;
}
static int serialization(void) {
    weather_batch b;
    weather_sample s = sample(WEATHER_TEMPERATURE, 1, 1, 300);
    char tiny[32];
    weather_batch_init(&b, UINT64_MAX);
    CHECK(weather_batch_json(&b, json, sizeof(json)) == -1 && !json[0]);
    weather_batch_prepare(&b);
    CHECK(weather_batch_json(&b, json, sizeof(json)) > 0);
    CHECK(strstr(json, "\"temperature\":[],\"humidity\":[]"));
    CHECK(strstr(json, "\"18446744073709551615\""));
    s.t_us = UINT64_MAX; CHECK(weather_batch_add(&b, &s)); CHECK(!b.prepared);
    CHECK(weather_batch_json(&b, json, sizeof(json)) == -1 && !json[0]);
    weather_batch_prepare(&b);
    CHECK(weather_batch_json(&b, tiny, sizeof(tiny)) == -1 && !tiny[0]);
    CHECK(weather_batch_json(&b, tiny, 1) == -1 && !tiny[0]);
    CHECK(weather_batch_json(&b, NULL, 0) == -1);
    CHECK(weather_batch_json(&b, json, sizeof(json)) > 0 && strstr(json, "\"t_us\":\"18446744073709551615\""));
    return 1;
}
static int position_provenance(void) {
    weather_batch b;
    weather_sample s=sample(WEATHER_TEMPERATURE,1,1,300);
    weather_batch_init(&b,0);
    s.position_kind=WEATHER_POSITION_ESTGPS; s.estimation_age_us=2000000;
    s.has_velocity=1; s.navigation_segment=7;
    CHECK(weather_sample_json(&s,json,sizeof(json))>0);
    CHECK(strstr(json,"\"quality\":268435456"));
    CHECK(strstr(json,"\"velocity_valid\":true,\"navigation_segment\":7"));
    CHECK(s.quality==0); /* Serializer normalizes its local copy only. */
    CHECK(weather_batch_add(&b,&s));
    CHECK(b.raw[0].quality & WEATHER_QUALITY_POSITION_ESTIMATED);
    CHECK(b.raw[0].estimation_age_us==2000000);
    s.position_kind=WEATHER_POSITION_GPS; s.estimation_age_us=1234; ++s.seq; ++s.t_us;
    CHECK(weather_batch_add(&b,&s));
    CHECK(b.group[0].representative.position_kind==WEATHER_POSITION_GPS);
    CHECK(b.raw[1].estimation_age_us==0);
    CHECK(!(b.raw[1].quality & WEATHER_QUALITY_POSITION_ESTIMATED));
    weather_batch_prepare(&b);
    CHECK(weather_batch_json(&b,json,sizeof(json))>0);
    CHECK(strstr(json,"\"position_source\":\"ESTGPS\",\"estimation_age_us\":2000000"));
    CHECK(strstr(json,"\"position_source\":\"GPS\",\"estimation_age_us\":0"));
    s.has_position=0; CHECK(!weather_sample_valid(&s));
    s.position_kind=WEATHER_POSITION_UNKNOWN; s.has_velocity=0; ++s.seq; ++s.t_us;
    CHECK(weather_batch_add(&b,&s));
    CHECK(b.raw[2].estimation_age_us==UINT32_MAX);
    weather_batch_prepare(&b); CHECK(weather_batch_json(&b,json,sizeof(json))>0);
    CHECK(strstr(json,"\"position_source\":\"UNKNOWN\",\"estimation_age_us\":null"));
    s.position_kind=3; CHECK(!weather_sample_valid(&s));
    return 1;
}
static int cost(void) {
    weather_batch b;
    weather_sample s = sample(WEATHER_TEMPERATURE, 1, 1, 300);
    const unsigned count = 200000;
    unsigned i;
    clock_t start = clock();
    weather_batch_init(&b, 0);
    for (i = 0; i < count; ++i) { s.seq = i; if (!weather_batch_add(&b, &s)) return 0; }
    printf("weather storage: batch=%zu sample=%zu group=%zu bytes; add mean=%.3f us (%u calls; average, not WCET)\n",
        sizeof(b), sizeof(s), sizeof(weather_group), (double)(clock() - start) * 1e6 / CLOCKS_PER_SEC / count, count);
    CHECK(b.n == count && b.raw_n == WEATHER_CAPACITY);
    return 1;
}
int main(void) {
    if (!grouping() || !protocol_identity() || !missing_time() || !representative() || !storage_and_numerics() || !limits() || !serialization() || !position_provenance() || !cost()) return 1;
    printf("weather: %u checks passed\n", checks);
    return 0;
}
