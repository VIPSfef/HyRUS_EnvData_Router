#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "qgc_bridge.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Waddress-of-packed-member"
#endif
#include "vendor/mavlink/mavlink_registry.h"
#include "vendor/mavlink/protocol.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET qb_socket;
#define QB_INVALID_SOCKET INVALID_SOCKET
static int qb_socket_error(void) { return WSAGetLastError(); }
static int qb_would_block(int code) { return code == WSAEWOULDBLOCK || code == WSAEINTR; }
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int qb_socket;
#define QB_INVALID_SOCKET (-1)
static int qb_socket_error(void) { return errno; }
static int qb_would_block(int code) { return code == EAGAIN || code == EWOULDBLOCK || code == EINTR; }
#endif

void qb_parser_init(qb_parser *parser)
{
    if (parser) memset(parser, 0, sizeof(*parser));
}

static size_t wire_frame(const mavlink_message_t *message, uint8_t *output)
{
    size_t length = mavlink_header_to_send_buffer(output, message);
    if (!length) return 0;
    /* mavlink_msg_to_send_buffer may trim payload zeroes. A relay must retain
     * the received length, checksum and signature, including nonminimal frames. */
    memcpy(output + length, _MAV_PAYLOAD(message), message->len);
    length += message->len;
    memcpy(output + length, message->ck, MAVLINK_NUM_CHECKSUM_BYTES);
    length += MAVLINK_NUM_CHECKSUM_BYTES;
    if (message->incompat_flags & MAVLINK_IFLAG_SIGNED) {
        memcpy(output + length, message->signature, MAVLINK_SIGNATURE_BLOCK_LEN);
        length += MAVLINK_SIGNATURE_BLOCK_LEN;
    }
    return length;
}

size_t qb_feed(qb_parser *parser, const uint8_t *bytes, size_t count,
               qb_emit_fn emit, void *context)
{
    size_t i, accepted = 0;
    if (!parser || (!bytes && count)) return 0;
    for (i = 0; i < count; ++i) {
        mavlink_status_t result;
        uint8_t framing = mavlink_frame_char_buffer(&parser->message,
            &parser->status, bytes[i], NULL, &result);
        parser->rejected += result.packet_rx_drop_count;
        if (framing == MAVLINK_FRAMING_OK) {
            uint8_t output[MAVLINK_MAX_PACKET_LEN];
            size_t length = wire_frame(&parser->message, output);
            if (!length) { ++parser->rejected; continue; }
            ++accepted;
            if (emit) emit(context, output, length);
        } else if (framing != MAVLINK_FRAMING_INCOMPLETE &&
                   parser->status.parse_state == MAVLINK_PARSE_STATE_IDLE) {
            /* A signed bad-CRC packet reports the error before and after its
             * signature. Count once, after the full frame has been consumed. */
            ++parser->rejected;
        }
    }
    return accepted;
}

static int fail(qb_bridge *bridge, const char *message, int code)
{
    ++bridge->report.errors;
    if (code) (void)snprintf(bridge->report.error, sizeof(bridge->report.error),
                             "%s (%d)", message, code);
    else (void)snprintf(bridge->report.error, sizeof(bridge->report.error), "%s", message);
    return 0;
}

void qb_init(qb_bridge *bridge)
{
    if (bridge) memset(bridge, 0, sizeof(*bridge));
}

void qb_close(qb_bridge *bridge)
{
    if (!bridge) return;
    if (bridge->socket_owned) {
#ifdef _WIN32
        (void)closesocket((SOCKET)bridge->socket_handle);
#else
        (void)close((int)bridge->socket_handle);
#endif
    }
#ifdef _WIN32
    if (bridge->network_started) (void)WSACleanup();
#endif
    bridge->socket_owned = bridge->network_started = 0;
    bridge->socket_handle = 0;
    bridge->report.opened = 0;
    bridge->report.local_port = 0;
    bridge->report.dropped += bridge->queue_count;
    bridge->queue_head = bridge->queue_count = 0;
    qb_parser_init(&bridge->parser);
}

int qb_open(qb_bridge *bridge, const char *host, unsigned port)
{
    struct sockaddr_in peer, local;
    qb_socket socket_handle;
#ifdef _WIN32
    int local_size = (int)sizeof(local);
#else
    socklen_t local_size = sizeof(local);
#endif
    if (!bridge) return 0;
    qb_close(bridge);
    memset(&peer, 0, sizeof(peer));
    peer.sin_family = AF_INET;
    if (!host || !port || port > UINT16_MAX || inet_pton(AF_INET, host, &peer.sin_addr) != 1)
        return fail(bridge, "QGC destination requires an IPv4 address and port 1..65535", 0);
    peer.sin_port = htons((uint16_t)port);
#ifdef _WIN32
    {
        WSADATA network;
        int code = WSAStartup(MAKEWORD(2, 2), &network);
        if (code) return fail(bridge, "Cannot initialize QGC UDP sockets", code);
        bridge->network_started = 1;
    }
#endif
    socket_handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_handle == QB_INVALID_SOCKET) {
        (void)fail(bridge, "Cannot create QGC UDP socket", qb_socket_error());
        qb_close(bridge);
        return 0;
    }
    bridge->socket_handle = (uintptr_t)socket_handle;
    bridge->socket_owned = 1;
#ifdef _WIN32
    {
        u_long nonblocking = 1;
        if (ioctlsocket(socket_handle, FIONBIO, &nonblocking) == SOCKET_ERROR) {
            (void)fail(bridge, "Cannot make QGC UDP socket nonblocking", qb_socket_error());
            qb_close(bridge);
            return 0;
        }
    }
#else
    {
        int flags = fcntl(socket_handle, F_GETFL, 0);
        if (flags < 0 || fcntl(socket_handle, F_SETFL, flags | O_NONBLOCK) < 0) {
            (void)fail(bridge, "Cannot make QGC UDP socket nonblocking", qb_socket_error());
            qb_close(bridge);
            return 0;
        }
    }
#endif
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(socket_handle, (const struct sockaddr *)&local, sizeof(local)) != 0 ||
        connect(socket_handle, (const struct sockaddr *)&peer, sizeof(peer)) != 0 ||
        getsockname(socket_handle, (struct sockaddr *)&local, &local_size) != 0) {
        (void)fail(bridge, "Cannot establish QGC UDP destination", qb_socket_error());
        qb_close(bridge);
        return 0;
    }
    bridge->report.local_port = ntohs(local.sin_port);
    bridge->report.opened = 1;
    bridge->report.error[0] = 0;
    return 1;
}

int qb_send(qb_bridge *bridge, const uint8_t *data, size_t length)
{
    int sent;
    if (!bridge) return 0;
    if (!bridge->report.opened || !data || !length || length > MAVLINK_MAX_PACKET_LEN) {
        ++bridge->report.dropped;
        return 0;
    }
    sent = (int)send((qb_socket)bridge->socket_handle, (const char *)data, (int)length, 0);
    if (sent != (int)length) {
        ++bridge->report.dropped;
        return fail(bridge, "QGC UDP send failed", sent < 0 ? qb_socket_error() : 0);
    }
    ++bridge->report.tx_frames;
    bridge->report.tx_bytes += length;
    bridge->report.error[0] = 0;
    return 1;
}

static void enqueue(void *context, const uint8_t *data, size_t length)
{
    qb_bridge *bridge = context;
    qb_frame *frame;
    if (bridge->queue_count == QB_QUEUE_CAPACITY) { ++bridge->report.dropped; return; }
    frame = &bridge->queue[(bridge->queue_head + bridge->queue_count) % QB_QUEUE_CAPACITY];
    memcpy(frame->data, data, length);
    frame->length = length;
    ++bridge->queue_count;
}

void qb_poll(qb_bridge *bridge)
{
    unsigned i;
    if (!bridge || !bridge->report.opened) return;
    for (i = 0; i < QB_POLL_DATAGRAMS; ++i) {
        int received = (int)recv((qb_socket)bridge->socket_handle,
            (char *)bridge->datagram, (int)sizeof(bridge->datagram), 0);
        uint64_t rejected;
        if (received < 0) {
            int code = qb_socket_error();
            if (!qb_would_block(code)) (void)fail(bridge, "QGC UDP receive failed", code);
            break;
        }
        if (!received) continue;
        rejected = bridge->parser.rejected;
        (void)qb_feed(&bridge->parser, bridge->datagram, (size_t)received, enqueue, bridge);
        bridge->report.dropped += bridge->parser.rejected - rejected;
    }
}

size_t qb_pop(qb_bridge *bridge, uint8_t *output, size_t capacity)
{
    qb_frame *frame;
    size_t length;
    if (!bridge || !output || !bridge->queue_count) return 0;
    frame = &bridge->queue[bridge->queue_head];
    length = frame->length;
    if (capacity < length) return 0;
    memcpy(output, frame->data, length);
    bridge->queue_head = (bridge->queue_head + 1u) % QB_QUEUE_CAPACITY;
    --bridge->queue_count;
    return length;
}

void qb_delivered(qb_bridge *bridge, size_t length)
{
    if (!bridge || !length) return;
    ++bridge->report.rx_frames;
    bridge->report.rx_bytes += length;
}
