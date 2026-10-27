#ifndef ROUTER_STATUS_REPORT_H
#define ROUTER_STATUS_REPORT_H

#include "monitor.h"

enum { SR_WIRE_CAP=16384 };

/* The dashboard and HTTP status use one receipt/source-state policy. */
const char *sr_receipt_state(int known,uint64_t received,uint64_t now,
                             unsigned timeout,const char *expired);
const char *sr_source_state(const tm_snapshot *snapshot,const tb_config *config);
int sr_position_known(const tm_snapshot *snapshot);
const char *sr_position_state(const tm_snapshot *snapshot,const tb_config *config);
const char *sr_position_source(const tm_snapshot *snapshot);
/* Diagnostic snapshots contain no new observations. Last measured values keep
 * their own receipt ages; all 64-bit counters/times are decimal strings. */
int sr_encode(const tm_snapshot *snapshot,const tb_config *config,
              const char *stream,const char *session,uint64_t generation,
              uint64_t sequence,char *output,size_t capacity);

#endif
