#ifndef UDP_BACKUP_H
#define UDP_BACKUP_H

#include <stddef.h>
#include <stdint.h>

enum { UB_DATAGRAM_CAP=1200, UB_HEADER_BYTES=40,
       UB_PAYLOAD_CAP=UB_DATAGRAM_CAP-UB_HEADER_BYTES,
       UB_JSON_CAP=2*1024*1024, UB_ASSEMBLY_CAP=4,
       UB_EXPIRE_MS=2000, UB_POLL_CAP=128 };
typedef struct {
    uint64_t messages,datagrams,bytes,errors,drops;
    unsigned last_error;
} ub_report;
typedef struct ub_sender ub_sender;
typedef struct ub_receiver ub_receiver;
typedef struct {
    uint8_t address[16]; /* Network-order IPv4 (first 4 bytes) or IPv6 bytes. */
    uint8_t family; /* 4 or 6; remaining bytes are ignored for IPv4. */
    uint16_t port; /* Host order. */
    uint32_t scope_id; /* IPv6 interface scope; ignored for IPv4. */
} ub_peer;
/* Complete, length-delimited JSON bytes only; the application validates JSON
 * syntax/schema. Storage is borrowed for this synchronous callback only. */
typedef void (*ub_message_fn)(void *context,const char *json,size_t length);

/* DNS is resolved once here, never on send. Errors use the platform socket
 * error code; a failed endpoint must not stop the application's other paths. */
ub_sender *ub_sender_open(const char *host,unsigned port,uint64_t run_nonce,unsigned *error);
int ub_sender_send(ub_sender *,const char *json,size_t length);
void ub_sender_get_report(const ub_sender *,ub_report *);
void ub_sender_close(ub_sender *);

ub_receiver *ub_receiver_open(const char *bind_host,unsigned port,
                             ub_message_fn,void *context,unsigned *error);
/* At most UB_POLL_CAP datagrams, no waits. now_ms is a monotonic host clock. */
void ub_receiver_poll(ub_receiver *,uint64_t now_ms);
void ub_receiver_get_report(const ub_receiver *,ub_report *);
void ub_receiver_close(ub_receiver *);

/* Socket-free framing/reassembly API, also used by deterministic tests. */
size_t ub_fragment(uint64_t run_nonce,uint64_t transfer_seq,const char *json,
                   size_t length,unsigned index,uint8_t *out,size_t capacity);
ub_receiver *ub_receiver_create(ub_message_fn,void *context);
void ub_receiver_feed(ub_receiver *,const ub_peer *,const uint8_t *,size_t,uint64_t now_ms);
void ub_receiver_expire(ub_receiver *,uint64_t now_ms);

#endif
