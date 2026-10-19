#ifndef ROUTER_INPUT_H
#define ROUTER_INPUT_H
#include "config.h"
#include "mavlink_input.h"
typedef struct {
    char source[256], status[32], error[160];
    uint64_t attempts, connections;
    mi_report mav;
} ri_report;
typedef void (*ri_status_fn)(void *, const ri_report *);
typedef int (*ri_stop_fn)(void *);
/* Long-running bounded receiver; no stdin, observation files or network sends
 * except a harmless 1Hz MAVLink heartbeat after a validated incoming frame. */
size_t ri_storage_bytes(void);
int ri_run(const tb_config *, mi_emit_fn, ri_status_fn, ri_stop_fn, void *);
#endif
