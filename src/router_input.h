#ifndef ROUTER_INPUT_H
#define ROUTER_INPUT_H
#include "config.h"
#include "mavlink_input.h"
#include "qgc_bridge.h"
#include "mavlink_rates.h"
typedef struct {
    char source[256], status[32], error[160];
    uint64_t attempts, connections, total_bytes, total_frames;
    /* mav belongs to the selected lane, including its final disconnect report.
     * False means a discovery candidate, never a newly selected device epoch. */
    int source_selected;
    unsigned serial_baud; /* Current/selected serial rate; zero for UDP/WS. */
    char qgc_status[32];
    qb_report qgc;
    mr_report rates;
    mi_report mav;
} ri_report;
typedef void (*ri_status_fn)(void *, const ri_report *);
typedef int (*ri_stop_fn)(void *);
/* Fixed two-lane discovery: UDP stays open while serial/WebSocket candidates
 * rotate; automatic discovery visits all serial ports at the configured baud
 * before trying each other supported baud, re-enumerating on every pass.
 * WebSocket is attempted only on the first baud pass. With auto_scan disabled,
 * the explicitly configured serial port/rate and WebSocket remain fixed.
 * The first CRC-valid stream is retained until no valid frame arrives within
 * link timeout. LOCKED additionally requires a fresh selected heartbeat;
 * other traffic keeps the channel open but reports HEARTBEAT_LOST once the
 * selected heartbeat expires. Data freshness is tracked independently. The lanes
 * never share decoder, timestamp or navigation state. Serial/WS read waits
 * and UDP read waits are capped at 20ms each; opening a configured WebSocket
 * may still wait connect_timeout_ms. No stdin or observation files. UDP input
 * sends nothing; the independent QGC bridge forwards only a selected serial
 * stream and returns QGC requests through the same serial write scheduler.
 * Serial auto-probes send no heartbeat before validation. Explicit
 * fixed serial/WS inputs may announce a 1Hz heartbeat before validation. The
 * opt-in rate controller only addresses a selected serial autopilot with a
 * fresh heartbeat, sharing the same complete-frame write scheduler. */
size_t ri_storage_bytes(void);
int ri_run(const tb_config *, mi_emit_fn, ri_status_fn, ri_stop_fn, void *);
#endif
