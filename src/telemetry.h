#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <stddef.h>
#include <stdint.h>

#ifndef TB_CAPACITY
#define TB_CAPACITY 512
#endif
#ifndef TB_MAX_KEYS
#define TB_MAX_KEYS 16
#endif
#ifndef TB_KEY_BYTES
#define TB_KEY_BYTES 32
#endif
#ifndef TB_UNIT_BYTES
#define TB_UNIT_BYTES 16
#endif
#ifndef TB_WINDOW_US
#define TB_WINDOW_US UINT64_C(1000000)
#endif
#ifndef TB_MAX_NAV_AGE_US
#define TB_MAX_NAV_AGE_US UINT32_C(200000)
#endif
#ifndef TB_MAX_TIME_UNCERT_US
#define TB_MAX_TIME_UNCERT_US UINT32_C(20000)
#endif
#if TB_CAPACITY < 1 || TB_CAPACITY > UINT32_MAX || TB_MAX_KEYS < 1 || TB_MAX_KEYS > UINT32_MAX
#error Capacities must fit positive uint32_t
#endif
#if TB_KEY_BYTES < 2 || TB_UNIT_BYTES < 2 || TB_WINDOW_US < 1
#error Text fields need room for a terminator; window must be positive
#endif

/* One synchronized numeric datum. t_us uses Pixhawk boot microseconds;
 * identify clock resets by a new session. utc_us=0 means UTC is unknown.
 * utc_uncert_us estimates absolute UTC error (UINT32_MAX means unknown);
 * time_uncert_us describes relative sensor/navigation timestamp alignment.
 * p: local ENU meters; v: ENU m/s; value may be NAN for a missing value.
 * key: [A-Za-z_][A-Za-z0-9_.-]*; unit: [A-Za-z0-9_./%*^+-]+.
 * Both strings must be NUL terminated in their fixed arrays.
 * quality is opaque; seq must increase per key, with a new session on wrap. */
typedef struct {
    uint64_t t_us;
    uint32_t seq, quality;
    char key[TB_KEY_BYTES], unit[TB_UNIT_BYTES];
    double value, p[3];
    float v[3];
    uint32_t time_uncert_us, nav_age_us;
    float pos_error_m;
    uint64_t utc_us;
    uint32_t utc_uncert_us;
} tb_sample;

typedef struct {
    char key[TB_KEY_BYTES], unit[TB_UNIT_BYTES];
    uint32_t n;
    double mean, m2, min, max;
    double mean_time_uncert_us, max_time_uncert_us;
    double mean_position_error_m, max_position_error_m;
    uint64_t last_t_us;
    uint32_t last_seq;
} tb_stat;

typedef struct {
    uint64_t window_us;
    uint32_t n, raw_n, raw_dropped, key_n;
    tb_stat stat[TB_MAX_KEYS];
    tb_sample raw[TB_CAPACITY];
    int prepared;
} tb_batch;

typedef struct {
    uint64_t t_us;
    double p[3];
    float v[3];
    float pos_error_m;
} tb_nav;

/* Allocation-free, no global state; caller provides thread synchronization. */
int tb_sample_valid(const tb_sample *sample);
void tb_batch_init(tb_batch *batch, uint64_t window_us);
/* 1 accepts, 0 rejects without mutation. Whole-second window; strictly
 * increasing time and seq PER KEY. A key has one unit per batch.
 * First TB_CAPACITY rows are retained; all accepted finite values update
 * Welford statistics. NaN updates ordering/counts but not the key's stat.n.
 * O(TB_MAX_KEYS), bounded text comparisons and at most one fixed row copy. */
int tb_batch_add(tb_batch *batch, const tb_sample *sample);
/* Worker-side allocation-free heapsort by (t_us,key,seq), O(raw_n log raw_n).
 * Required after the last add and before either JSON function. At saturation
 * the retained subset depends on arrival order; statistics still include all. */
void tb_batch_prepare(tb_batch *batch);
/* Returns bytes excluding trailing NUL, or -1 on invalid state/truncation.
 * Failure output MUST NOT be sent. Caller must use the C numeric locale.
 * Raw rows have 17 fields:
 * [t_us,seq,quality,key,unit,px,py,pz,vx,vy,vz,value,
 *  time_uncert_us,nav_age_us,pos_error_m,utc_us,utc_uncert_us].
 * t_us/window_us/known utc_us are decimal strings; unknown UTC is null.
 * utc_uncert_us is null when utc_us is zero; otherwise it is an integer.
 * NaN value is null; stat.n=0 has null mean/variance/min/max; n<2 has null
 * sample variance. Uncertainty statistics use the same finite-value samples;
 * estimated position error = pos_error_m + |v|*time_uncert_us/1e6 is a
 * current-speed estimate, not an absolute bound. Statistics are sorted by key.
 * API-created batches only. */
int tb_batch_json(const tb_batch *batch, char *buf, size_t cap);
/* include_raw=0 emits raw:[], raw_included:false; raw_n is still retained rows. */
int tb_batch_json_mode(const tb_batch *batch, char *buf, size_t cap,
                       int include_raw);
/* Interpolate bracketing navigation in the SAME clock/session, no extrapolation;
 * span <= TB_MAX_NAV_AGE_US. Exact equal timestamps select a. accel_bound must
 * be finite in [0,1e6] m/s^2. Error is interpolated endpoint error + a*dt^2/8,
 * rounded upwards to float. No velocity-error guarantee is made.
 * Success changes only out.t_us,p,v,nav_age_us,pos_error_m; failure changes
 * nothing. Timestamp uncertainty and key/value/UTC metadata are preserved. */
int tb_align(const tb_nav *a, const tb_nav *b, uint64_t t_us,
             double accel_bound_mps2, tb_sample *out);

#endif
