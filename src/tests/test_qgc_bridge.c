/* Protocol identity and real loopback transport checks. No aircraft, serial
 * ports, QGC instance or observation files are used by this executable. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "qgc_bridge.h"
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET test_socket;
typedef int address_length;
#define TEST_INVALID_SOCKET INVALID_SOCKET
static void socket_close(test_socket handle) { (void)closesocket(handle); }
static void pause_ms(unsigned ms) { Sleep(ms); }
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
typedef int test_socket;
typedef socklen_t address_length;
#define TEST_INVALID_SOCKET (-1)
static void socket_close(test_socket handle) { (void)close(handle); }
static void pause_ms(unsigned ms)
{
    struct timespec duration = {(time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L};
    while (nanosleep(&duration, &duration) && errno == EINTR) {}
}
#endif

enum { TEST_MAGIC_V1 = 0xfe, TEST_MAGIC_V2 = 0xfd, TEST_HEARTBEAT_CRC = 50,
       TEST_POLL_ATTEMPTS = 100, TEST_PAUSE_MS = 2, TEST_CAPTURE_CAPACITY = 8 };
static unsigned checks;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); return 0; \
} } while (0)

typedef struct { qb_frame frame[TEST_CAPTURE_CAPACITY]; size_t count; } capture;
static void captured(void *context, const uint8_t *bytes, size_t length)
{
    capture *result = context;
    if (result->count < TEST_CAPTURE_CAPACITY) {
        result->frame[result->count].length = length;
        memcpy(result->frame[result->count].data, bytes, length);
    }
    ++result->count;
}
static uint16_t crc_byte(uint16_t crc, uint8_t value)
{
    uint8_t tmp = (uint8_t)(value ^ crc);
    tmp ^= (uint8_t)(tmp << 4);
    return (uint16_t)((crc >> 8) ^ ((uint16_t)tmp << 8) ^ ((uint16_t)tmp << 3) ^ (tmp >> 4));
}
static size_t packet(uint8_t *out, int v2, unsigned flags, unsigned payload_length,
                     uint32_t message_id, uint8_t extra)
{
    size_t i, length = 0;
    uint16_t crc = UINT16_MAX;
    out[length++] = v2 ? TEST_MAGIC_V2 : TEST_MAGIC_V1;
    out[length++] = (uint8_t)payload_length;
    if (v2) { out[length++] = (uint8_t)flags; out[length++] = 0x80; }
    out[length++] = 42; /* Sequence must survive, including signed frames. */
    out[length++] = 17;
    if (v2 && (flags & MAVLINK_IFLAG_SYSID32)) {
        out[length++] = 0x23; out[length++] = 0x45; out[length++] = 0x67;
    }
    out[length++] = 9;
    out[length++] = (uint8_t)message_id;
    if (v2) { out[length++] = (uint8_t)(message_id >> 8); out[length++] = (uint8_t)(message_id >> 16); }
    if (v2 && (flags & MAVLINK_IFLAG_TARGET32)) {
        out[length++] = 0x12; out[length++] = 0x34; out[length++] = 0x56; out[length++] = 0x78;
    }
    /* Deliberately retain trailing zeroes: re-finalizing would invalidate CRC. */
    for (i = 0; i < payload_length; ++i) out[length++] = i + 2u < payload_length ? (uint8_t)i : 0;
    for (i = 1; i < length; ++i) crc = crc_byte(crc, out[i]);
    crc = crc_byte(crc, extra);
    out[length++] = (uint8_t)crc; out[length++] = (uint8_t)(crc >> 8);
    if (v2 && (flags & MAVLINK_IFLAG_SIGNED))
        for (i = 0; i < MAVLINK_SIGNATURE_BLOCK_LEN; ++i) out[length++] = (uint8_t)(0xa0u + i);
    return length;
}

static int parser_preserves_frames(void)
{
    qb_parser parser, other;
    capture result = {0}, other_result = {0};
    uint8_t wire[MAVLINK_MAX_PACKET_LEN];
    size_t n, split;
    qb_parser_init(&parser); qb_parser_init(&other);
    n = packet(wire, 0, 0, 9, 0, TEST_HEARTBEAT_CRC);
    CHECK(qb_feed(&parser, wire, n, captured, &result) == 1);
    CHECK(result.count == 1 && result.frame[0].length == n && !memcmp(result.frame[0].data, wire, n));
    n = packet(wire, 1, MAVLINK_IFLAG_SIGNED, 9, 0, TEST_HEARTBEAT_CRC);
    split = n / 2u;
    CHECK(qb_feed(&parser, wire, split, captured, &result) == 0);
    CHECK(qb_feed(&other, wire, n, captured, &other_result) == 1);
    CHECK(qb_feed(&parser, wire + split, n - split, captured, &result) == 1);
    CHECK(result.count == 2 && result.frame[1].length == n && !memcmp(result.frame[1].data, wire, n));
    CHECK(other_result.count == 1 && !memcmp(other_result.frame[0].data, wire, n));
    n = packet(wire, 1, MAVLINK_IFLAG_SIGNED | MAVLINK_IFLAG_SYSID32 | MAVLINK_IFLAG_TARGET32,
               MAVLINK_MAX_PAYLOAD_LEN, 0, TEST_HEARTBEAT_CRC);
    CHECK(n == MAVLINK_MAX_PACKET_LEN);
    CHECK(qb_feed(&parser, wire, n, captured, &result) == 1);
    CHECK(result.count == 3 && result.frame[2].length == n && !memcmp(result.frame[2].data, wire, n));
    CHECK(!parser.rejected && !other.rejected);
    return 1;
}

static int parser_rejects_and_resets(void)
{
    qb_parser parser;
    capture result = {0};
    uint8_t wire[MAVLINK_MAX_PACKET_LEN];
    size_t n;
    qb_parser_init(&parser);
    n = packet(wire, 1, MAVLINK_IFLAG_SIGNED, 9, 0, TEST_HEARTBEAT_CRC);
    wire[10] ^= 1u;
    CHECK(!qb_feed(&parser, wire, n, captured, &result));
    CHECK(parser.rejected == 1 && !result.count);
    n = packet(wire, 1, 0, 9, UINT32_C(0xffffff), 0);
    CHECK(!qb_feed(&parser, wire, n, captured, &result));
    CHECK(parser.rejected == 2 && !result.count);
    n = packet(wire, 1, 0, 9, 0, TEST_HEARTBEAT_CRC);
    CHECK(qb_feed(&parser, wire, n, captured, &result) == 1);
    CHECK(result.count == 1 && !memcmp(result.frame[0].data, wire, n));
    CHECK(!qb_feed(&parser, wire, n / 2u, captured, &result));
    qb_parser_init(&parser);
    CHECK(!qb_feed(&parser, wire + n / 2u, n - n / 2u, captured, &result));
    CHECK(qb_feed(&parser, wire, n, captured, &result) == 1);
    CHECK(result.count == 2);
    return 1;
}

static test_socket listener(struct sockaddr_in *address)
{
    test_socket handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    address_length length = (address_length)sizeof(*address);
    if (handle == TEST_INVALID_SOCKET) return handle;
    memset(address, 0, sizeof(*address));
    address->sin_family = AF_INET;
    address->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(handle, (const struct sockaddr *)address, sizeof(*address)) ||
        getsockname(handle, (struct sockaddr *)address, &length)) {
        socket_close(handle); return TEST_INVALID_SOCKET;
    }
#ifdef _WIN32
    {
        u_long nonblocking = 1;
        if (ioctlsocket(handle, FIONBIO, &nonblocking)) { socket_close(handle); return TEST_INVALID_SOCKET; }
    }
#else
    if (fcntl(handle, F_SETFL, O_NONBLOCK)) { socket_close(handle); return TEST_INVALID_SOCKET; }
#endif
    return handle;
}

static int loopback_transport(void)
{
    static qb_bridge bridge;
    struct sockaddr_in qgc_address, foreign_address, bridge_address;
    test_socket qgc, foreign;
    uint8_t wire[MAVLINK_MAX_PACKET_LEN], received[MAVLINK_MAX_PACKET_LEN];
    uint8_t bundle[(QB_QUEUE_CAPACITY + 1u) * MAVLINK_MAX_PACKET_LEN];
    size_t n, i;
    int count = -1;
    address_length address_size = (address_length)sizeof(bridge_address);
    qgc = listener(&qgc_address); foreign = listener(&foreign_address);
    CHECK(qgc != TEST_INVALID_SOCKET && foreign != TEST_INVALID_SOCKET);
    qb_init(&bridge);
    CHECK(qb_open(&bridge, "127.0.0.1", ntohs(qgc_address.sin_port)));
    CHECK(bridge.report.opened && bridge.report.local_port && bridge.report.local_port != ntohs(qgc_address.sin_port));
    n = packet(wire, 1, MAVLINK_IFLAG_SIGNED, 9, 0, TEST_HEARTBEAT_CRC);
    CHECK(qb_send(&bridge, wire, n));
    for (i = 0; i < TEST_POLL_ATTEMPTS && count < 0; ++i) {
        count = (int)recvfrom(qgc, (char *)received, (int)sizeof(received), 0,
            (struct sockaddr *)&bridge_address, &address_size);
        if (count < 0) pause_ms(TEST_PAUSE_MS);
    }
    CHECK(count == (int)n && !memcmp(received, wire, n));
    CHECK(ntohs(bridge_address.sin_port) == bridge.report.local_port);
    CHECK(bridge.report.tx_frames == 1 && bridge.report.tx_bytes == n);
    CHECK(sendto(foreign, (const char *)wire, (int)n, 0, (const struct sockaddr *)&bridge_address,
                 sizeof(bridge_address)) == (int)n);
    for (i = 0; i < 5; ++i) { qb_poll(&bridge); pause_ms(TEST_PAUSE_MS); }
    CHECK(!qb_pop(&bridge, received, sizeof(received)));
    CHECK(sendto(qgc, (const char *)wire, (int)n, 0, (const struct sockaddr *)&bridge_address,
                 sizeof(bridge_address)) == (int)n);
    for (i = 0; i < TEST_POLL_ATTEMPTS && !bridge.queue_count; ++i) { qb_poll(&bridge); pause_ms(TEST_PAUSE_MS); }
    CHECK(bridge.queue_count == 1 && !bridge.report.rx_frames);
    CHECK(!qb_pop(&bridge, received, n - 1u) && bridge.queue_count == 1);
    CHECK(qb_pop(&bridge, received, sizeof(received)) == n && !memcmp(received, wire, n));
    CHECK(!bridge.report.rx_frames);
    qb_delivered(&bridge, n);
    CHECK(bridge.report.rx_frames == 1 && bridge.report.rx_bytes == n);
    for (i = 0; i < QB_QUEUE_CAPACITY + 1u; ++i) memcpy(bundle + i * n, wire, n);
    CHECK(sendto(qgc, (const char *)bundle, (int)((QB_QUEUE_CAPACITY + 1u) * n), 0,
                 (const struct sockaddr *)&bridge_address, sizeof(bridge_address)) == (int)((QB_QUEUE_CAPACITY + 1u) * n));
    for (i = 0; i < TEST_POLL_ATTEMPTS && !bridge.queue_count; ++i) { qb_poll(&bridge); pause_ms(TEST_PAUSE_MS); }
    CHECK(bridge.queue_count == QB_QUEUE_CAPACITY && bridge.report.dropped == 1);
    for (i = 0; i < QB_QUEUE_CAPACITY; ++i) {
        CHECK(qb_pop(&bridge, received, sizeof(received)) == n);
        CHECK(!memcmp(received, wire, n));
    }
    CHECK(!qb_pop(&bridge, received, sizeof(received)));
    qb_close(&bridge); qb_close(&bridge);
    CHECK(!bridge.report.opened && !bridge.report.local_port && !bridge.queue_count);
    CHECK(bridge.report.tx_frames == 1 && bridge.report.rx_frames == 1);
    CHECK(qb_open(&bridge, "127.0.0.1", ntohs(qgc_address.sin_port)));
    CHECK(!bridge.queue_count && !bridge.parser.rejected);
    qb_close(&bridge);
    socket_close(qgc); socket_close(foreign);
    return 1;
}

int main(void)
{
    int passed;
#ifdef _WIN32
    WSADATA network;
    if (WSAStartup(MAKEWORD(2, 2), &network)) return 1;
#endif
    passed = parser_preserves_frames() && parser_rejects_and_resets() && loopback_transport();
#ifdef _WIN32
    (void)WSACleanup();
#endif
    if (!passed) return 1;
    printf("qgc_bridge: %u checks passed\n", checks);
    return 0;
}
