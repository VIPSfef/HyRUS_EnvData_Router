#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <stddef.h>
#include <stdint.h>

#define TB_CHANNELS 4
#ifndef TB_CAPACITY
#define TB_CAPACITY 128
#endif
#if TB_CAPACITY < 1 || TB_CAPACITY > UINT32_MAX
#error TB_CAPACITY must fit a positive uint32_t
#endif
#define TB_WINDOW_US UINT64_C(1000000)
#ifndef TB_MAX_NAV_AGE_US
#define TB_MAX_NAV_AGE_US UINT32_C(200000)
#endif

/* One sensor/session. Time is synchronized measurement time in microseconds;
 * p is local ENU meters; v is ENU meters/second, aligned with that time.
 * value order: temperature Celsius, RH percent, PM2.5 and PM10 ug/m^3.
 * Missing channel values are NAN; quality is an opaque caller flag mask.
 * Sequence numbers must increase; start a new session before uint32 wrap. */
typedef struct {
    uint64_t t_us;
    uint32_t seq, quality;
    double p[3];
    float v[3], value[TB_CHANNELS];
    uint32_t nav_age_us;
} tb_sample;

typedef struct {
    uint32_t n;
    double mean, m2, min, max;
} tb_stat;

typedef struct {
    uint64_t window_us;
    uint32_t n, raw_n, raw_dropped;
    tb_stat stat[TB_CHANNELS];
    tb_sample raw[TB_CAPACITY];
    uint64_t last_t_us;
    uint32_t last_seq;
} tb_batch;

/* All functions are allocation-free. Add is O(TB_CHANNELS); JSON is
 * O(TB_CAPACITY). Caller owns synchronization and must use the C numeric
 * locale for JSON. No global state is modified. */
int tb_sample_valid(const tb_sample *sample);
void tb_batch_init(tb_batch *batch, uint64_t window_us);
/* 1 accepts, 0 rejects without mutation. Window is [window_us, +1 second),
 * aligned to whole seconds. Strictly increasing times and seq are required.
 * First TB_CAPACITY samples are retained; later samples still update stats. */
int tb_batch_add(tb_batch *batch, const tb_sample *sample);
/* Returns bytes excluding trailing NUL, or -1 on bad arguments/truncation.
 * A failed output is incomplete and MUST NOT be sent. Timestamp is a decimal
 * string to retain all uint64 bits. Raw rows have 14 flat fields:
 * [t_us,seq,quality,px,py,pz,vx,vy,vz,temp,rh,pm25,pm10,nav_age_us].
 * NaN is JSON null. Stats use unbiased sample variance (m2/(n-1)); empty
 * stats have null mean/variance/min/max, and n=1 has null variance.
 * Serialize only batches initialized/updated through this API. */
int tb_batch_json(const tb_batch *batch, char *buf, size_t cap);
/* include_raw=0 emits raw:[] and raw_included:false; raw_n still reports
 * retained samples. Nonzero includes the retained rows. */
int tb_batch_json_mode(const tb_batch *batch, char *buf, size_t cap,
                       int include_raw);

#endif
