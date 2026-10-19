#ifndef ROUTER_INPUT_H
#define ROUTER_INPUT_H
#include "config.h"
#include "mavlink_input.h"
typedef struct {
    char source[256], status[32], error[160];
    uint64_t attempts, connections, total_bytes, total_frames;
    mi_report mav;
} ri_report;
typedef void (*ri_status_fn)(void *, const ri_report *);
typedef int (*ri_stop_fn)(void *);
/* Fixed two-lane discovery: UDP stays open while serial/WebSocket candidates
 * rotate; the first CRC-valid stream is retained until link timeout. The lanes
 * never share decoder, timestamp or navigation state. Serial/WS read waits
 * and UDP read waits are capped at 20ms each; opening a configured WebSocket
 * may still wait connect_timeout_ms. No stdin or observation files. UDP sends
 * nothing; serial/WS may announce a 1Hz heartbeat on trusted/validated input. */
size_t ri_storage_bytes(void);
int ri_run(const tb_config *, mi_emit_fn, ri_status_fn, ri_stop_fn, void *);
#endif
