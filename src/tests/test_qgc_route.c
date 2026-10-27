/* Real router + official wire parser + real loopback UDP, with mocked serial.
 * No hardware is opened and every actual socket uses an ephemeral test port.
 * Flow: reject probe -> select valid stream -> retain first frame -> serialize
 * partial heartbeat/request writes -> close/reset -> reconnect without leakage. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "router_input.h"
#include "input_transport.h"
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET test_socket;
typedef int address_length;
#define INVALID_TEST_SOCKET INVALID_SOCKET
static void socket_close(test_socket s) { (void)closesocket(s); }
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
#define INVALID_TEST_SOCKET (-1)
static void socket_close(test_socket s) { (void)close(s); }
static void pause_ms(unsigned ms)
{
    struct timespec wait = {(time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L};
    while (nanosleep(&wait, &wait) && errno == EINTR) {}
}
#endif

enum { ROUTE_V1, ROUTE_SIGNED, ROUTE_RECONNECT, ROUTE_UDP };
enum { FRAME_CAPACITY = MAVLINK_MAX_PACKET_LEN, SERIAL_CAPACITY = 1024,
       CONNECTION_CAPACITY = 2, REQUEST_COUNT = 4, HEARTBEAT_CRC = 50,
       PARAM_REQUEST_LIST_ID = 21, PARAM_REQUEST_LIST_CRC = 159 };
typedef struct { uint8_t data[FRAME_CAPACITY]; size_t length; } frame;
typedef struct {
    unsigned mode, errors, reads, writes, connections, resets, probe_reads, premature_writes;
    int selected, delivered, disconnect, done, udp_delivered;
    test_socket socket;
    struct sockaddr_in address, peer;
    frame vehicle, heartbeat, request[REQUEST_COUNT];
    uint8_t serial[CONNECTION_CAPACITY][SERIAL_CAPACITY];
    size_t serial_length[CONNECTION_CAPACITY];
    unsigned forwarded;
    ri_report report;
} fixture;
static fixture mock;
static unsigned checks;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
    fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#condition); return 0; \
} } while (0)

static uint16_t crc_byte(uint16_t crc, uint8_t value)
{
    uint8_t t = (uint8_t)(value ^ crc);
    t ^= (uint8_t)(t << 4);
    return (uint16_t)((crc >> 8) ^ ((uint16_t)t << 8) ^ ((uint16_t)t << 3) ^ (t >> 4));
}
/* Independent wire fixture: no production finalize/pack function is used. */
static void packet(frame *f, int signed_v2, uint8_t sequence, uint8_t system,
                   uint8_t component, uint8_t id, const uint8_t *payload,
                   size_t payload_length, uint8_t extra)
{
    size_t i, n = 0;
    uint16_t crc = UINT16_MAX;
    f->data[n++] = signed_v2 ? 0xfd : 0xfe;
    f->data[n++] = (uint8_t)payload_length;
    if (signed_v2) { f->data[n++] = 1; f->data[n++] = 0; }
    f->data[n++] = sequence; f->data[n++] = system; f->data[n++] = component;
    f->data[n++] = id;
    if (signed_v2) { f->data[n++] = 0; f->data[n++] = 0; }
    memcpy(f->data + n, payload, payload_length); n += payload_length;
    for (i = 1; i < n; ++i) crc = crc_byte(crc, f->data[i]);
    crc = crc_byte(crc, extra);
    f->data[n++] = (uint8_t)crc; f->data[n++] = (uint8_t)(crc >> 8);
    if (signed_v2)
        for (i = 0; i < 13; ++i) f->data[n++] = (uint8_t)(0xb0u + i);
    f->length = n;
}

static int open_listener(void)
{
    address_length size = (address_length)sizeof(mock.address);
    mock.socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (mock.socket == INVALID_TEST_SOCKET) return 0;
    memset(&mock.address, 0, sizeof(mock.address));
    mock.address.sin_family = AF_INET;
    mock.address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(mock.socket, (const struct sockaddr *)&mock.address, sizeof(mock.address)) ||
        getsockname(mock.socket, (struct sockaddr *)&mock.address, &size)) return 0;
#ifdef _WIN32
    {
        u_long nonblocking = 1;
        return ioctlsocket(mock.socket, FIONBIO, &nonblocking) == 0;
    }
#else
    return fcntl(mock.socket, F_SETFL, O_NONBLOCK) == 0;
#endif
}
static void send_reply(const uint8_t *data, size_t length)
{
    int n = (int)sendto(mock.socket, (const char *)data, (int)length, 0,
                       (const struct sockaddr *)&mock.peer, sizeof(mock.peer));
    if (n != (int)length) ++mock.errors;
}
/* Called from mocked reads, after the router has had a chance to send. */
static void service_qgc(void)
{
    uint8_t bytes[4 * FRAME_CAPACITY];
    address_length size = (address_length)sizeof(mock.peer);
    int n = (int)recvfrom(mock.socket, (char *)bytes, (int)sizeof(bytes), 0,
                         (struct sockaddr *)&mock.peer, &size);
    size_t length = 0, split = mock.request[2].length / 2u;
    if (n < 0) return;
    ++mock.forwarded;
    if (!mock.selected || n != (int)mock.vehicle.length ||
        memcmp(bytes, mock.vehicle.data, mock.vehicle.length)) { ++mock.errors; return; }
    if (mock.mode == ROUTE_RECONNECT && mock.connections == 2) {
        memcpy(bytes, mock.request[2].data + split, mock.request[2].length - split);
        length = mock.request[2].length - split;
        memcpy(bytes + length, mock.request[3].data, mock.request[3].length);
        length += mock.request[3].length;
    } else {
        memcpy(bytes, mock.request[0].data, mock.request[0].length);
        length = mock.request[0].length;
        memcpy(bytes + length, mock.request[1].data, mock.request[1].length);
        length += mock.request[1].length;
        if (mock.mode == ROUTE_RECONNECT) {
            memcpy(bytes + length, mock.request[2].data, split);
            length += split;
        }
    }
    send_reply(bytes, length);
}

size_t it_enumerate(char endpoints[][IT_ENDPOINT_BYTES], size_t capacity)
{
    size_t n = 0;
    if (mock.mode == ROUTE_V1 && n < capacity)
        (void)snprintf(endpoints[n++], IT_ENDPOINT_BYTES, "TEST_BAD_CRC");
    if (n < capacity) (void)snprintf(endpoints[n++], IT_ENDPOINT_BYTES, "TEST_SERIAL");
    return n;
}
int it_open(it_handle *h, it_kind kind, const char *endpoint, unsigned baud,
            unsigned connect_timeout_ms, unsigned io_timeout_ms)
{
    (void)baud; (void)connect_timeout_ms; (void)io_timeout_ms;
    memset(h, 0, sizeof(*h));
    h->kind = kind; h->opened = 1;
    if (kind == IT_UDP && mock.mode == ROUTE_UDP) { h->fd = 2; return 1; }
    if (kind != IT_SERIAL || mock.mode == ROUTE_UDP) { ++mock.errors; return 0; }
    h->fd = !strcmp(endpoint, "TEST_BAD_CRC") ? 0 : 1;
    if (h->fd) {
        if (++mock.connections > CONNECTION_CAPACITY) { ++mock.errors; return 0; }
        mock.delivered = mock.selected = mock.disconnect = 0;
    }
    return 1;
}
int it_read(it_handle *h, void *output, size_t capacity)
{
    ++mock.reads;
    service_qgc();
    if (!h->opened || capacity < mock.vehicle.length) { ++mock.errors; return -1; }
    if (h->fd == 0) {
        if (!mock.probe_reads++) {
            memcpy(output, mock.vehicle.data, mock.vehicle.length);
            ((uint8_t *)output)[mock.vehicle.length - 1u] ^= 1u;
            return (int)mock.vehicle.length;
        }
        pause_ms(3); return 0;
    }
    if (h->fd == 2) {
        if (!mock.udp_delivered++) {
            memcpy(output, mock.vehicle.data, mock.vehicle.length);
            return (int)mock.vehicle.length;
        }
        return 0;
    }
    if (mock.disconnect) {
        (void)snprintf(h->error, sizeof(h->error), "Fixture serial disconnected");
        return -1;
    }
    if (!mock.delivered++) {
        memcpy(output, mock.vehicle.data, mock.vehicle.length);
        return (int)mock.vehicle.length;
    }
    pause_ms(1);
    return 0;
}
int it_write(it_handle *h, const void *data, size_t length)
{
    size_t take, *used;
    unsigned connection;
    if (!h->opened || h->fd != 1 || !mock.selected) {
        ++mock.premature_writes; return -1;
    }
    connection = mock.connections - 1u;
    used = &mock.serial_length[connection];
    take = 1u + mock.writes++ % 3u;
    if (take > length) take = length;
    if (*used + take > SERIAL_CAPACITY) { ++mock.errors; return -1; }
    memcpy(mock.serial[connection] + *used, data, take); *used += take;
    if (mock.mode == ROUTE_RECONNECT && connection == 0 && *used > mock.heartbeat.length)
        mock.disconnect = 1;
    else {
        size_t expected = mock.heartbeat.length;
        if (mock.mode == ROUTE_RECONNECT) expected += mock.request[3].length;
        else expected += mock.request[0].length + mock.request[1].length;
        if (*used >= expected) mock.done = 1;
    }
    return (int)take;
}
void it_close(it_handle *h) { h->opened = 0; }
void it_udp_probe_reset(it_handle *h) { (void)h; ++mock.errors; }
const char *it_error(const it_handle *h) { return h->error; }

static void emitted(void *context, const mi_event *event)
{
    fixture *f = context;
    if (event->kind == MI_RESET) {
        ++f->resets; f->selected = 1;
        if (f->mode == ROUTE_UDP) f->done = 1;
    }
}
static void reported(void *context, const ri_report *report)
{
    fixture *f = context;
    f->report = *report;
}
static int stop_requested(void *context)
{
    const fixture *f = context;
    return f->errors || f->done;
}
static int begin_fixture(tb_config *c, unsigned mode)
{
    static const uint8_t heartbeat_payload[] = {0, 0, 0, 0, 1, 12, 29, 4, 3};
    uint8_t request_payload[] = {42, 1};
    unsigned i;
    memset(&mock, 0, sizeof(mock)); mock.mode = mode;
    CHECK(open_listener());
    cfg_defaults(c);
    c->udp_enabled = mode == ROUTE_UDP;
    c->udp_port = ntohs(mock.address.sin_port);
    c->qgc_port = ntohs(mock.address.sin_port);
    c->auto_scan = mode != ROUTE_UDP;
    (void)snprintf(c->serial_port, sizeof(c->serial_port), "%s", mode == ROUTE_UDP ? "none" : "auto");
    c->probe_ms = 2; c->reconnect_ms = 1; c->io_timeout_ms = 1;
    c->link_timeout_ms = 2000; c->duration_ms = 1000;
    packet(&mock.vehicle, mode == ROUTE_SIGNED, 7, 42, 1, 0,
           heartbeat_payload, sizeof(heartbeat_payload), HEARTBEAT_CRC);
    mock.heartbeat.length = mi_heartbeat(mock.heartbeat.data, sizeof(mock.heartbeat.data), 255, 191, 0);
    CHECK(mock.heartbeat.length > 0);
    for (i = 0; i < REQUEST_COUNT; ++i) {
        request_payload[1] = (uint8_t)(i + 1u);
        packet(&mock.request[i], 0, (uint8_t)(10u + i), 255, 190,
               PARAM_REQUEST_LIST_ID, request_payload, sizeof(request_payload), PARAM_REQUEST_LIST_CRC);
    }
    return 1;
}
static int run_fixture(const tb_config *c)
{
    int success = ri_run(c, emitted, reported, stop_requested, &mock);
    service_qgc();
    socket_close(mock.socket); mock.socket = INVALID_TEST_SOCKET;
    if (!success || !mock.done || mock.errors || mock.premature_writes)
        fprintf(stderr,"mode=%u done=%d errors=%u early_writes=%u connections=%u forwarded=%u writes=%u serial=%zu/%zu qgc=%s tx=%llu drop=%llu error=%s\n",
                mock.mode,mock.done,mock.errors,mock.premature_writes,mock.connections,mock.forwarded,
                mock.writes,mock.serial_length[0],mock.serial_length[1],mock.report.qgc_status,
                (unsigned long long)mock.report.qgc.tx_frames,(unsigned long long)mock.report.qgc.dropped,mock.report.qgc.error);
    CHECK(success && mock.done && !mock.errors && !mock.premature_writes);
    CHECK(!strcmp(mock.report.status, "STOPPED"));
    CHECK(!mock.report.qgc.opened && !mock.report.qgc.local_port);
    return 1;
}
static int bidirectional_partial_writes(unsigned mode)
{
    tb_config c;
    size_t position;
    CHECK(begin_fixture(&c, mode));
    CHECK(run_fixture(&c));
    CHECK(mock.resets == 1 && mock.report.connections == 1 && mock.forwarded == 1);
    CHECK(mock.report.qgc.tx_frames == 1 && mock.report.qgc.tx_bytes == mock.vehicle.length);
    CHECK(mock.report.qgc.rx_frames == 2 && mock.report.qgc.rx_bytes == mock.request[0].length + mock.request[1].length);
    CHECK(!mock.report.qgc.errors && !mock.report.qgc.dropped);
    position = mock.heartbeat.length;
    CHECK(!memcmp(mock.serial[0], mock.heartbeat.data, position));
    CHECK(!memcmp(mock.serial[0] + position, mock.request[0].data, mock.request[0].length));
    position += mock.request[0].length;
    CHECK(!memcmp(mock.serial[0] + position, mock.request[1].data, mock.request[1].length));
    CHECK(mock.serial_length[0] == position + mock.request[1].length);
    CHECK(mock.writes > 3);
    if (mode == ROUTE_V1) CHECK(mock.probe_reads > 1 && mock.report.total_frames == 1);
    else CHECK(mock.report.mav.signed_rejected == 1 && !mock.report.mav.frames && mock.report.total_frames == 1);
    return 1;
}
static int reconnect_isolates_pending_requests(void)
{
    tb_config c;
    frame next_heartbeat;
    size_t prefix;
    CHECK(begin_fixture(&c, ROUTE_RECONNECT));
    CHECK(run_fixture(&c));
    CHECK(mock.resets == 2 && mock.report.connections == 2 && mock.forwarded == 2);
    CHECK(mock.report.qgc.tx_frames == 2 && mock.report.qgc.rx_frames == 1);
    CHECK(mock.report.qgc.rx_bytes == mock.request[3].length);
    CHECK(mock.report.qgc.dropped == 2 && !mock.report.qgc.errors);
    prefix = mock.serial_length[0] - mock.heartbeat.length;
    CHECK(prefix > 0 && prefix < mock.request[0].length);
    CHECK(!memcmp(mock.serial[0], mock.heartbeat.data, mock.heartbeat.length));
    CHECK(!memcmp(mock.serial[0] + mock.heartbeat.length, mock.request[0].data, prefix));
    CHECK(mock.serial_length[1] == mock.heartbeat.length + mock.request[3].length);
    next_heartbeat.length = mi_heartbeat(next_heartbeat.data, sizeof(next_heartbeat.data), 255, 191, 1);
    CHECK(next_heartbeat.length == mock.heartbeat.length);
    CHECK(!memcmp(mock.serial[1], next_heartbeat.data, next_heartbeat.length));
    CHECK(!memcmp(mock.serial[1] + mock.heartbeat.length, mock.request[3].data, mock.request[3].length));
    return 1;
}
static int udp_input_does_not_forward(void)
{
    tb_config c;
    CHECK(begin_fixture(&c, ROUTE_UDP));
    CHECK(run_fixture(&c));
    CHECK(mock.report.connections == 1 && mock.report.total_frames == 1);
    CHECK(!mock.connections && !mock.forwarded && !mock.writes);
    CHECK(!mock.report.qgc.tx_frames && !mock.report.qgc.rx_frames && !mock.report.qgc.errors);
    CHECK(!strcmp(mock.report.qgc_status, "WAIT_SERIAL"));
    return 1;
}
int main(void)
{
    int success;
#ifdef _WIN32
    WSADATA network;
    if (WSAStartup(MAKEWORD(2, 2), &network)) return 1;
#endif
    success = bidirectional_partial_writes(ROUTE_V1) && bidirectional_partial_writes(ROUTE_SIGNED) &&
              reconnect_isolates_pending_requests() && udp_input_does_not_forward();
#ifdef _WIN32
    (void)WSACleanup();
#endif
    if (!success) return 1;
    printf("QGC router: %u checks passed\n", checks);
    return 0;
}
