#ifndef QGC_BRIDGE_H
#define QGC_BRIDGE_H

#include <stddef.h>
#include <stdint.h>
/* The upstream protocol types contain anonymous unions for their public ABI. */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
#include "vendor/mavlink/mavlink_types.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

enum { QB_QUEUE_CAPACITY = 32, QB_POLL_DATAGRAMS = 8, QB_DATAGRAM_CAPACITY = 65507 };

typedef void (*qb_emit_fn)(void *, const uint8_t *, size_t);
typedef struct {
    mavlink_message_t message;
    mavlink_status_t status;
    uint64_t rejected;
} qb_parser;

/* CRC validation covers the bundled official dialect registry. Signing is not
 * authenticated by this relay; complete signatures are forwarded unchanged for
 * the endpoints to verify. Parsers belong to one stream and one owning thread. */
void qb_parser_init(qb_parser *parser);
size_t qb_feed(qb_parser *, const uint8_t *, size_t, qb_emit_fn, void *);

typedef struct {
    uint64_t tx_frames, tx_bytes, rx_frames, rx_bytes, dropped, errors;
    unsigned local_port;
    int opened;
    char error[160];
} qb_report;

typedef struct {
    size_t length;
    uint8_t data[MAVLINK_MAX_PACKET_LEN];
} qb_frame;

typedef struct {
    qb_report report;
    qb_parser parser;
    uintptr_t socket_handle;
    int socket_owned, network_started;
    unsigned queue_head, queue_count;
    qb_frame queue[QB_QUEUE_CAPACITY];
    uint8_t datagram[QB_DATAGRAM_CAPACITY];
} qb_bridge;

/* All calls run in the receiver thread. The connected, nonblocking IPv4 socket
 * binds an ephemeral local port, never the QGC destination port. Kernel peer
 * filtering accepts replies only from the configured destination. */
void qb_init(qb_bridge *bridge);
int qb_open(qb_bridge *, const char *host, unsigned port);
void qb_close(qb_bridge *bridge);
int qb_send(qb_bridge *, const uint8_t *, size_t);
void qb_poll(qb_bridge *bridge);
/* Removes one complete frame. Insufficient capacity leaves it queued. */
size_t qb_pop(qb_bridge *, uint8_t *, size_t capacity);
/* Count QGC-to-serial delivery only after the caller completes every byte. */
void qb_delivered(qb_bridge *, size_t length);

#endif
