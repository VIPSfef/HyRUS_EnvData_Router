#ifndef INPUT_TRANSPORT_H
#define INPUT_TRANSPORT_H
#include <stddef.h>
#include <stdint.h>
#define IT_ENDPOINT_BYTES 256u
#define IT_WEBSOCKET_ENDPOINT_BYTES 2048u
#define IT_MAX_CANDIDATES 64u
#define IT_MAX_UDP_DATAGRAM 65507u
/* Every instance has bounded storage. Treat the members as private. */
typedef enum { IT_SERIAL = 1, IT_WEBSOCKET = 2, IT_UDP = 3 } it_kind;
typedef struct {
    int kind, opened, fd, fragmented;
    unsigned io_timeout_ms;
    uintptr_t udp_socket;
    int udp_owned, udp_started, udp_peer_set;
    uint32_t udp_peer_address;
    uint16_t udp_peer_port;
    size_t udp_buffered, udp_offset;
    unsigned char udp_buffer[IT_MAX_UDP_DATAGRAM];
    void *session, *connection, *socket, *worker, *ready, *consumed, *stop;
    volatile long worker_result;
    size_t buffered, offset, header_n, header_need, control_n;
    uint64_t frame_left;
    unsigned opcode, frame_fin;
    unsigned char buffer[8192], header[14], control[125];
    char error[256];
} it_handle;
/* Enumeration is stable and bounded. No device is opened by enumeration. */
size_t it_enumerate(char endpoints[][IT_ENDPOINT_BYTES], size_t capacity);
/* Initialize/close must be called by the same owning input thread.
   IT_UDP binds an IPv4 literal:port exclusively (no port sharing). The first
   nonempty datagram latches its source address/port until close/reopen. */
int it_open(it_handle *h, it_kind kind, const char *endpoint, unsigned baud,
            unsigned connect_timeout_ms, unsigned io_timeout_ms);
/* Positive byte count; 0 means idle; -1 means disconnect/error. Maximum wait
   per read is 100 ms. WebSocket accepts only binary MAVLink byte streams.
   UDP retains a complete <=65,507-byte datagram across smaller reads; only
   the latched peer is delivered. Datagram boundaries are not emitted. */
int it_read(it_handle *h, void *output, size_t capacity);
/* Small outbound frames (e.g. TIMESYNC). Partial serial writes are reported.
   UDP is receive-only: it_write returns 0 without sending any data. */
int it_write(it_handle *h, const void *data, size_t length);
void it_close(it_handle *h);
const char *it_error(const it_handle *h);
#endif