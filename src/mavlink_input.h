#ifndef MAVLINK_INPUT_H
#define MAVLINK_INPUT_H

#include "telemetry.h"
#include "weather.h"
#include <stddef.h>
#include <stdint.h>

#define MI_DEVICE_CAP 32u
#define MI_NAV_CAP 32u
#define MI_PENDING_CAP 512u
#define MI_REORDER_US UINT64_C(200000)
#define MI_QUALITY_POSITION_UNKNOWN UINT32_C(0x40000000)
#define MI_QUALITY_CLOCK_UNSIGNED UINT32_C(0x20000000)
#define MI_QUALITY_ESTGPS WEATHER_QUALITY_POSITION_ESTIMATED
/* Conservative policy, not an error bound. No yaw/airspeed extrapolation. */
#define MI_GPS_FIX_FRESH_MS UINT64_C(2000)
#define MI_EST_STATUS_FRESH_US UINT64_C(2000000)
#define MI_EST_MAX_AGE_US UINT64_C(30000000)
#define MI_EST_VELOCITY_GAP_US UINT64_C(500000)
#define MI_GLOBAL_FRESH_MS UINT64_C(500)
#define MI_EST_VELOCITY_MAX_MPS 500.0
#define MI_ESTGPS_NO_ANCHOR UINT32_C(1)
#define MI_ESTGPS_STATUS_INVALID UINT32_C(2)
#define MI_ESTGPS_VELOCITY_INVALID UINT32_C(4)
#define MI_ESTGPS_VELOCITY_GAP UINT32_C(8)
#define MI_ESTGPS_EXPIRED UINT32_C(16)
enum { MI_ESTIMATE_NONE = 0, MI_ESTIMATE_EKF = 1, MI_ESTIMATE_VELOCITY = 2 };

/* Passive decoding is CRC validated, but not authenticated. MAVLink signing
 * needs a secret and anti-replay state; signed frames are rejected by default. */
typedef struct {
    uint8_t target_system, target_component; /* 0: first autopilot heartbeat */
    uint8_t allow_signed_unverified, origin_auto;
    double origin_lat, origin_lon, origin_alt; /* WGS84 deg, MSL meters */
    uint32_t system_time_uncert_us; /* UINT32_MAX: source has no error guarantee */
    double accel_bound_mps2; /* interpolation assumption, explicit not measured */
} mi_options;

typedef struct {
    uint8_t sysid, compid, type, autopilot, base_mode, system_status;
    uint32_t custom_mode;
    uint64_t last_seen_ms, last_heartbeat_ms, packets, sequence_gaps;
    uint8_t have_heartbeat, have_sequence, last_sequence;
} mi_device;

typedef struct {
    uint64_t bytes, frames, bad_crc, unknown_message, bad_length;
    uint64_t unsupported_flags, signed_rejected, sequence_gaps, duplicates;
    uint64_t observations, no_navigation, no_clock, wrong_clock_component;
    uint64_t invalid_value, pending_overflow, late_observation, nav_rejected;
    uint64_t device_overflow, reboots, sequence_wraps, weather_reboot_dropped;
    uint32_t devices, pending;
    uint8_t selected_system, selected_component, gps_fix;
    uint64_t boot_us, watermark_us;
    double lat, lon, altitude_msl_m;
    uint64_t navigation_received_ms, navigation_boot_us;
    uint8_t have_navigation;
    uint8_t position_kind, gps_fix_fresh, have_gps_anchor, estimate_source;
    uint64_t gps_fix_received_ms, last_fix_boot_us;
    uint32_t estimation_age_us, position_flags;
    float navigation_error_m, navigation_v[3];
    mi_device device[MI_DEVICE_CAP];
} mi_report;

typedef enum { MI_SAMPLE, MI_CLOCK, MI_RESET, MI_WATERMARK, MI_ORIGIN, MI_WEATHER_SAMPLE } mi_event_kind;
typedef struct {
    mi_event_kind kind;
    uint8_t sysid, compid;
    uint32_t epoch;
    tb_sample sample;
    weather_sample weather;
    uint64_t boot_us, utc_us;
    uint32_t uncert_us;
    double origin[3]; /* MI_ORIGIN: WGS84 lat/lon, MSL altitude */
} mi_event;
typedef void (*mi_emit_fn)(void *context, const mi_event *event);

typedef struct {
    tb_sample sample;
    uint64_t received_ms;
    uint8_t aligned; /* cached before high-rate navigation history is evicted */
} mi_pending;
typedef struct {
    uint64_t anchor_us;
    uint32_t segment;
    uint8_t position_kind, have_anchor, source;
} mi_nav_info;

typedef struct {
    mi_options options;
    mi_report report;
    mi_emit_fn emit;
    void *context;
    uint8_t frame[280];
    size_t used, needed;
    tb_nav nav[MI_NAV_CAP];
    mi_nav_info nav_info[MI_NAV_CAP]; /* Parallel metadata; no tb_nav ABI change. */
    unsigned nav_count;
    mi_pending pending[MI_PENDING_CAP];
    unsigned pending_count;
    struct { weather_sample sample; uint64_t received_ms; uint8_t aligned; } weather_pending[WEATHER_CAPACITY];
    unsigned weather_pending_count;
    uint32_t weather_seq, navigation_segment;
    uint64_t high_boot_us, wrap_ms, last_emitted_us, last_watermark_us;
    uint32_t last_boot_ms, epoch, next_seq, emitted_seq;
    uint8_t have_boot, have_emitted, origin_set;
    float gps_error_m;
    uint64_t gps_accuracy_ms;
    uint64_t gps_anchor_us, gps_anchor_received_ms, gps_fix_boot_us;
    uint64_t global_boot_us, global_received_ms;
    uint64_t local_velocity_us, local_velocity_received_ms;
    uint64_t previous_velocity_us, previous_velocity_received_ms;
    uint64_t estimate_boot_us, estimator_boot_us, estimator_received_ms;
    double estimate_p[3], meridian_m, east_radius_m;
    float local_velocity[3], previous_velocity[3];
    uint16_t estimator_flags;
    uint8_t have_gps_fix, have_gps_anchor, have_global, have_local_velocity;
    uint8_t estimate_active, have_estimator_status, have_previous_velocity, have_gps_fix_clock;
    uint8_t absolute_invalid_active;
} mi_input;

/* Fixed storage, no allocation, feed is O(bytes * log(common messages) +
 * packets * (MI_DEVICE_CAP + MI_PENDING_CAP * MI_PENDING_CAP +
 * MI_PENDING_CAP * MI_NAV_CAP)); all caps fixed.
 * Call from the receiver thread only. Callback is synchronous. */
void mi_options_default(mi_options *options);
void mi_init(mi_input *input, const mi_options *options,
             mi_emit_fn emit, void *context);
void mi_feed(mi_input *input, const uint8_t *bytes, size_t count,
             uint64_t host_monotonic_ms);
void mi_tick(mi_input *input, uint64_t host_monotonic_ms);
/* Flush the final reorder tail at a controlled stream boundary. Only samples
 * on the legacy path require actual bracketing navigation; otherwise they are
 * counted and discarded. Weather values are preserved with unknown positions
 * when navigation is unavailable. Call before changing channels or ending a session. */
void mi_finish(mi_input *input, uint64_t host_monotonic_ms);
void mi_get_report(const mi_input *input, mi_report *report);
/* Discard a partial frame when switching byte streams. Device/clock history and
 * the configured coordinate origin stay; estimator/GPS anchors and navigation
 * history are cleared so a different byte source cannot continue dead reckoning.
 * Call mi_finish first to preserve the old source's pending observations. */
void mi_link_reset(mi_input *input);
/* Optional announcement (not a command): MAV_TYPE_ONBOARD_CONTROLLER and
 * MAV_AUTOPILOT_INVALID, MAVLink v1, 17 bytes. Returns 0 for bad IDs/capacity. */
size_t mi_heartbeat(uint8_t *out, size_t capacity, uint8_t sysid,
                    uint8_t compid, uint8_t sequence);
#endif