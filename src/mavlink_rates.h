#ifndef MAVLINK_RATES_H
#define MAVLINK_RATES_H

#include "mavlink_input.h"
#include <stddef.h>
#include <stdint.h>

typedef enum {
    MR_DISABLED, MR_WAIT_LINK, MR_OBSERVING, MR_APPLYING, MR_MONITORING,
    MR_CONGESTED, MR_WAIT_GCS, MR_PROBING, MR_HOLD
} mr_state;
typedef struct {
    mr_state state;
    unsigned budget_bps, observed_bps, planned_bps;
    unsigned rate_scale_milli; /* Inferred common actual/requested rate; 1000 = 1.0. */
    uint64_t commands, accepted, unsupported, timed_out;
    uint64_t probes, probe_rollbacks;
    int radio_txbuf; /* -1: no recent RADIO_STATUS */
} mr_report;

enum { MR_STREAM_CAP = 32, MR_OBSERVE_MS = 3000, MR_WINDOW_MS = 5000,
       MR_ACK_MS = 2000, MR_PACE_MS = 500, MR_MAX_ATTEMPTS = 2,
       MR_GCS_PAUSE_MS = 5000, MR_PROBE_MS = 20000,
       MR_PROBE_SETTLE_MS = 5000, MR_SCALE_WINDOW_MS = 15000 };
typedef struct {
    uint32_t desired_us, applied_us;
    unsigned observed, window_frames, observed_millihz, window_bytes;
    unsigned average_millihz, average_bps;
    uint64_t applied_at_ms, scale_ms, scale_frames, scale_at_ms;
    unsigned scale_milli;
    uint8_t blocked;
} mr_stream;
typedef struct {
    mr_report report;
    mr_stream stream[MR_STREAM_CAP];
    uint8_t enabled, sysid, compid, weather_choice, airspeed_choice;
    unsigned uart_ceiling_bps, pending, attempts, queued, scan;
    uint32_t pending_interval_us;
    uint64_t window_ms, window_bytes;
    uint64_t last_packets, last_gaps, reboots, sent_ms, next_command_ms;
    uint64_t pause_until_ms, radio_received_ms;
    unsigned have_radio, window_unmanaged_bytes, congested;
    unsigned average_unmanaged_bps, have_measurement, window_control_bytes, planned_budget_bps;
    unsigned planned_rate_scale_milli;
    uint64_t stable_bytes, stable_primary_frames, stable_ms;
    uint64_t probe_bytes, probe_primary_frames, probe_ms, probe_started_ms;
    uint64_t faults_seen;
    unsigned probe_active, probe_hold, probe_previous_budget;
    unsigned baseline_bps, baseline_primary_millihz;
    unsigned held_bps, held_primary_millihz;
    uint32_t observed_mask, held_mask;
} mr_controller;

const char *mr_state_name(mr_state state);
void mr_init(mr_controller *, int enabled, unsigned baud);
/* Only CRC-validated frames from this serial lane. Signed control replies are
 * ignored because this host has no signing key. Other traffic still costs bytes. */
void mr_observe(mr_controller *, const uint8_t *, size_t, uint64_t now_ms);
/* Call for complete outgoing QGC frames before they own the serial scheduler.
 * Conflicting rate requests cancel our pending attribution and temporarily yield. */
void mr_external(mr_controller *, const uint8_t *, size_t, uint64_t now_ms);
void mr_tick(mr_controller *, const mi_device *, uint64_t reboots, uint64_t now_ms,
             unsigned heartbeat_timeout_ms);
/* A single whole frame owns serial output. mr_sent starts its ACK deadline only
 * after the scheduler has written all bytes; a partial write cannot be retried. */
size_t mr_next(mr_controller *, uint8_t sequence, uint8_t *, size_t, uint64_t now_ms);
void mr_sent(mr_controller *, uint64_t now_ms);

#endif
