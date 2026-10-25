#ifndef SPATIAL_H
#define SPATIAL_H

#include "weather.h"
#include <stddef.h>
#include <stdint.h>

#define SP_GROUP_CAP 16u
#define SP_COMPLETED_CAP 128u
#define SP_BATCH_CAP (SP_COMPLETED_CAP + SP_GROUP_CAP)
#define SP_MAX_WIDTH_M 1000000000.0
#define SP_FLAG_UNDERSAMPLED UINT32_C(1)
#define SP_FLAG_MOTION_UNKNOWN UINT32_C(2)
#define SP_FLAG_NAV_UNKNOWN UINT32_C(4)
#define SP_FLAG_VELOCITY_UNKNOWN UINT32_C(8)
#define SP_FLAG_TIME_GAP UINT32_C(16)
#define SP_FLAG_CONTEXT_CHANGE UINT32_C(32)
#define SP_FLAG_SOURCE_CHANGE UINT32_C(64)
#define SP_FLAG_OUT_OF_ORDER UINT32_C(128)
#define SP_FLAG_RATE_LIMITED UINT32_C(256)
#define SP_FLAG_COUNT_OVERFLOW UINT32_C(512)
#define SP_FLAG_CONFIG_CHANGE UINT32_C(1024)
#define SP_FLAG_PATH_ESTIMATED UINT32_C(2048)
enum { SP_PARTIAL = 0, SP_COMPLETE = 1, SP_INTERRUPTED = 2 };

typedef struct {
    double step_m; /* target, default 10 m; effective width is explicit per bin */
    uint64_t gap_us; /* acquisition gap policy, default 2 seconds */
    double chord_tolerance_m, chord_relative_tolerance; /* quality heuristic, not accuracy bounds */
    int adaptive;
} sp_options;

typedef struct {
    uint64_t continuity_key, path_id, bin_index, revision;
    uint64_t t_start_us, t_end_us;
    uint32_t message_id, sensor_id, n, flags, quality;
    double mean, m2, min, max;
    double s_start_m, s_end_m; /* actual retained-observation path coordinates */
    double interval_start_m, interval_end_m, width_m, entry_step_m;
    weather_sample representative; /* one actual observation; never a synthetic boundary sample */
    uint8_t metric, sensor_instance, source_kind, status;
} sp_record;

typedef struct {
    uint64_t samples_seen, spatial_samples, unknown_samples, rejected_samples, late_samples;
    uint64_t paths_started, completed_records, dropped_records, undersampled_steps, inconsistent_steps;
    uint64_t path_id, bin_index;
    double s_m, effective_width_m;
    uint32_t flags, open_n, completed_n;
    uint8_t path_known;
} sp_report;

typedef struct {
    sp_options options;
    sp_report report;
    uint32_t n;
    sp_record record[SP_BATCH_CAP];
} sp_batch;

typedef struct {
    sp_record record;
    uint64_t emitted_revision;
} sp_open;

typedef struct {
    sp_options options;
    sp_report report;
    sp_open open[SP_GROUP_CAP];
    sp_record completed[SP_COMPLETED_CAP];
    uint32_t open_n, completed_head, completed_n;
    uint64_t continuity_key, last_t_us, path_id, bin_index;
    double s_m, s_compensation, interval_start_m, width_m, entry_step_m;
    double anchor_p[2], predicted_displacement[2], last_v[2], last_p[2];
    uint32_t path_flags, bin_flags, shrink_credit;
    uint8_t have_context, have_step, source_kind, capacity_pressure, current_step_valid;
} sp_state;
typedef sp_state spatial_state;
typedef sp_batch spatial_batch;

void sp_options_default(sp_options *options);
int sp_options_valid(const sp_options *options);
void sp_init(sp_state *state, const sp_options *options);
/* Receiver-thread/caller-mutex only. Feed every accepted observation exactly
 * once, in nondecreasing acquisition time. Same-time metric/sensor pairs share
 * one horizontal path step. continuity_key must change for source/session/
 * boot/origin/nav-segment changes. velocity_valid is acquisition-time evidence,
 * not merely finite v. Returns 1 aggregated, 0 spatially unknown/rejected.
 * O(SP_GROUP_CAP) fixed work and storage; no heap, loops over distance, missing
 * observations, generated boundary values, or fabricated empty bins. */
int sp_add(sp_state *state, const weather_sample *sample, uint64_t continuity_key, int velocity_valid);
/* Close nonempty bins as interrupted. Does not reset path IDs/counters. */
void sp_break(sp_state *state, uint32_t reason);
/* Changes apply to the next path; invalid settings leave state unchanged. */
int sp_set_options(sp_state *state, const sp_options *options);
/* Copies completed records plus dirty partial revisions, then consumes that
 * local completed queue. Transport must own an immutable sp_batch copy until
 * ACK. Does NOT reset an open bin or its statistics on a transmission tick.
 * Upsert key: sender/session + continuity_key + path_id + bin_index +
 * metric/message_id/sensor_id/sensor_instance/source_kind. Revision is cumulative
 * per group/bin; never sum repeated partials or a completed replacement. */
void sp_take_snapshot(sp_state *state, sp_batch *batch);
/* Whole mavlink_spatial_v1 object; failure clears out[0]. No partial send.
 * Serialization is worker-side O(record count), separately from sp_add. */
int sp_batch_json(const sp_batch *batch, char *out, size_t capacity);

#endif
