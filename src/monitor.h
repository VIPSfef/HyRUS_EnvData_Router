#ifndef TELEMETRY_MONITOR_H
#define TELEMETRY_MONITOR_H

#include <stdio.h>
#include "config.h"
#include "telemetry.h"
#include "time_sync.h"

#define TM_MAX_DEVICES 32u
/* Explicit location provenance; zero-initialized snapshots remain unknown. */
enum { TM_POSITION_UNKNOWN=0, TM_POSITION_GPS=1, TM_POSITION_ESTGPS=2,
       TM_POSITION_NO_ANCHOR=3 };
typedef struct {
    uint8_t sysid, compid, type, autopilot, base_mode, system_status, have_heartbeat;
    uint32_t custom_mode;
    uint64_t last_seen_ms;
} tm_device;
typedef struct {
    char key[TB_KEY_BYTES], unit[TB_UNIT_BYTES];
    uint64_t observations, t_us, last_receipt_ms;
    uint32_t seq;
    double value;
    int have_value;
} tm_key;
typedef struct {
    uint64_t now_ms, started_ms;
    int stop, eof, failed, dry_run, test_mode;
    uint64_t input_bytes, input_lines, accepted, rejected, sequence_gaps;
    uint64_t raw_dropped, queue_windows_dropped, queue_samples_dropped;
    unsigned queue_count, queue_capacity, current_raw, raw_capacity;
    unsigned send_capacity, inflight_windows;
    uint64_t watermark_us, current_window_us;
    unsigned current_observations;
    uint64_t oldest_age_ms;
    uint64_t net_attempts, net_acks, net_failures, dry_releases, net_sent_bytes;
    unsigned consecutive_failures;
    long http_status;
    unsigned error_code;
    uint64_t last_attempt_ms, last_success_ms, last_receipt_ms;
    uint64_t last_anchor_receipt_ms, last_boot_us;
    double last_rtt_ms, last_encode_ms;
    unsigned key_n;
    tm_key keys[TB_MAX_KEYS];
    int have_position, have_clock;
    double p[3];
    float v[3];
    uint32_t time_uncert_us, nav_age_us, utc_uncert_us;
    float pos_error_m;
    uint64_t utc_us;
    tc_report clock;
    char active_source[256], source_status[32], source_error[160];
    uint64_t parsed_frames, bad_crc, unknown_message, no_navigation;
    uint64_t pending_overflow, signed_rejected, weather_reboot_dropped;
    uint64_t no_clock, wrong_clock_component, invalid_value, late_observation;
    unsigned device_n, pending_observations, selected_system, selected_component, gps_fix;
    tm_device devices[TM_MAX_DEVICES];
    /* Display metadata only. Estimated locations must never masquerade as GPS.
     * Populate explicit known flags; zero is a valid coordinate/age/error value.
     * Invalid or absent flags render unknown, never (0,0) by default. */
    unsigned position_kind;
    int have_geodetic, have_fix_age, have_position_error;
    double latitude_deg, longitude_deg, altitude_msl_m, estimated_error_m;
    uint64_t last_fix_age_ms;
    int spatial_known;
    double spatial_effective_m, spatial_distance_m;
    uint64_t spatial_completed, spatial_unknown, spatial_dropped, spatial_undersampled;
    uint32_t spatial_flags, spatial_open;
} tm_snapshot;

/* tm_snapshot stays below 64 KiB (enforced in monitor.c).
 * Root owns thread synchronization; snapshot returns a bounded consistent copy.
 * Active configuration is immutable. Private draft saves apply on restart.
 * UDP bind/port/enabled are editable source settings with the same save rules.
 * Keyboard is CONIN$ or /dev/tty. With CSV console stdin, editing is disabled;
 * MAVLink input does not consume stdin, so console editing remains available.
 * stderr receives the dashboard, or plain 1 Hz snapshots when redirected.
 * Only a live LOCKED MAVLink source is green; waiting/probing/disconnected
 * sources are yellow. Redirected diagnostics never emit colour escapes. */
void tm_run(void *context, void (*snapshot)(void *, tm_snapshot *),
            const tb_config *active, const char *config_path);
/* One plain snapshot; no terminal input or writes. URL queries and credentials
 * are hidden. Useful for tests or a one-shot diagnostic. */
void tm_render(FILE *out, const tm_snapshot *snapshot, const tb_config *active);

#endif
