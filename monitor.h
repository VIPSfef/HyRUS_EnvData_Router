#ifndef TELEMETRY_MONITOR_H
#define TELEMETRY_MONITOR_H

#include <stdio.h>
#include "config.h"
#include "telemetry.h"
#include "time_sync.h"

typedef struct {
    char key[TB_KEY_BYTES], unit[TB_UNIT_BYTES];
    uint64_t observations, t_us;
    uint32_t seq;
    double value;
    int have_value;
} tm_key;

typedef struct {
    uint64_t now_ms, started_ms;
    int stop, eof, failed, dry_run;
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
} tm_snapshot;

/* Root owns the thread and synchronization; snapshot must return a consistent
 * bounded copy and set stop when monitoring should finish. Active configuration
 * is immutable. Editing only changes a private draft; save applies on restart.
 * Keyboard input uses CONIN$ or /dev/tty, never the CSV input stream.
 * Console drawing goes to stderr; redirected stderr gets plain 1 Hz snapshots.
 * This module does not stop the receiver or change any process-global locale. */
void tm_run(void *context, void (*snapshot)(void *, tm_snapshot *),
            const tb_config *active, const char *config_path);
/* One plain snapshot; no escapes, terminal input or configuration writes.
 * Does not display an endpoint or credential value. Useful for --monitor-once. */
void tm_render(FILE *out, const tm_snapshot *snapshot, const tb_config *active);

#endif