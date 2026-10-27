/* Exercise the real router/decoder with a fully in-memory transport. No serial
 * device, socket, filesystem input, or worker thread is opened by these tests.
 * Flow under test: enumerate -> preferred-rate pass -> fallback passes -> CRC
 * validation -> selection -> disconnect -> fresh preferred-rate enumeration. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "router_input.h"
#include "input_transport.h"
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <time.h>
#endif

enum { PORT_CAPACITY = 4, ATTEMPT_CAPACITY = 128, PACKET_CAPACITY = 32 };
enum { DEFAULT_BAUD = 115200, FALLBACK_BAUD = 57600, FIXED_BAUD = 38400 };
typedef enum { RX_IDLE, RX_HEARTBEAT, RX_BAD_CRC, RX_NOISE } receive_kind;
typedef struct {
    char endpoint[IT_ENDPOINT_BYTES];
    unsigned baud, reads, writes;
    int opened, closed, delivered;
    receive_kind receive;
} attempt;
typedef struct {
    const char *ports[PORT_CAPACITY], *replacement[PORT_CAPACITY];
    size_t port_n, replacement_n;
    const char *valid_port, *replacement_valid_port, *missing_port, *busy_port;
    const char *noise_port, *bad_crc_port;
    unsigned valid_baud, replacement_baud, idle_ms;
    unsigned enumerations, attempt_n, resets, stop_connections, stop_attempts;
    unsigned transport_errors, premature_writes;
    int reconnect, replaced;
    uint8_t packet[PACKET_CAPACITY];
    size_t packet_n;
    attempt attempts[ATTEMPT_CAPACITY];
    ri_report final_report;
} fixture;

static fixture mock;
static unsigned checks;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); return 0; \
} } while (0)

static int matches(const char *a, const char *b) { return a && b && !strcmp(a, b); }
static void pause_ms(unsigned ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec wait = {(time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L};
    while (nanosleep(&wait, &wait) && errno == EINTR) {}
#endif
}

size_t it_enumerate(char endpoints[][IT_ENDPOINT_BYTES], size_t capacity)
{
    size_t i, n = mock.replaced ? mock.replacement_n : mock.port_n;
    const char **ports = mock.replaced ? mock.replacement : mock.ports;
    ++mock.enumerations;
    if (n > capacity) n = capacity;
    for (i = 0; i < n; ++i) (void)snprintf(endpoints[i], IT_ENDPOINT_BYTES, "%s", ports[i]);
    return n;
}

int it_open(it_handle *h, it_kind kind, const char *endpoint, unsigned baud,
            unsigned connect_timeout_ms, unsigned io_timeout_ms)
{
    attempt *a;
    const char *valid = mock.replaced ? mock.replacement_valid_port : mock.valid_port;
    unsigned valid_baud = mock.replaced ? mock.replacement_baud : mock.valid_baud;
    (void)connect_timeout_ms;
    memset(h, 0, sizeof(*h));
    h->kind = kind;
    h->io_timeout_ms = io_timeout_ms;
    if (kind != IT_SERIAL || mock.attempt_n >= ATTEMPT_CAPACITY) {
        ++mock.transport_errors;
        (void)snprintf(h->error, sizeof(h->error), "Unexpected mock transport open");
        return 0;
    }
    h->fd = (int)mock.attempt_n;
    a = &mock.attempts[mock.attempt_n++];
    (void)snprintf(a->endpoint, sizeof(a->endpoint), "%s", endpoint);
    a->baud = baud;
    if (matches(endpoint, mock.missing_port) || matches(endpoint, mock.busy_port)) {
        (void)snprintf(h->error, sizeof(h->error), "%s",
                       matches(endpoint, mock.busy_port) ? "Port busy" : "Port disappeared");
        return 0;
    }
    a->receive = matches(endpoint, valid) && baud == valid_baud ? RX_HEARTBEAT :
        matches(endpoint, mock.bad_crc_port) ? RX_BAD_CRC :
        matches(endpoint, mock.noise_port) ? RX_NOISE : RX_IDLE;
    h->opened = a->opened = 1;
    return 1;
}

static attempt *open_attempt(it_handle *h)
{
    if (!h->opened || h->fd < 0 || (unsigned)h->fd >= mock.attempt_n) {
        ++mock.transport_errors;
        return NULL;
    }
    return &mock.attempts[(unsigned)h->fd];
}

int it_read(it_handle *h, void *output, size_t capacity)
{
    static const unsigned char noise[] = {0x12, 0x34, 0x56, 0x78};
    attempt *a = open_attempt(h);
    if (!a) return -1;
    ++a->reads;
    if (!a->delivered && a->receive != RX_IDLE) {
        size_t n = a->receive == RX_NOISE ? sizeof(noise) : mock.packet_n;
        if (capacity < n) { ++mock.transport_errors; return -1; }
        memcpy(output, a->receive == RX_NOISE ? noise : mock.packet, n);
        if (a->receive == RX_BAD_CRC) ((uint8_t *)output)[n - 1u] ^= 1u;
        a->delivered = 1;
        return (int)n;
    }
    if (a->receive == RX_HEARTBEAT && mock.reconnect && !mock.replaced) {
        mock.replaced = 1;
        (void)snprintf(h->error, sizeof(h->error), "Mock cable unplugged");
        return -1;
    }
    pause_ms(mock.idle_ms);
    return 0;
}

int it_write(it_handle *h, const void *data, size_t length)
{
    attempt *a = open_attempt(h);
    (void)data;
    if (!a) return -1;
    ++a->writes;
    if (a->receive != RX_HEARTBEAT || !a->delivered) ++mock.premature_writes;
    return (int)length;
}

void it_udp_probe_reset(it_handle *h) { (void)h; ++mock.transport_errors; }
void it_close(it_handle *h)
{
    attempt *a = open_attempt(h);
    if (a) { a->closed = 1; h->opened = 0; }
}
const char *it_error(const it_handle *h) { return h->error; }

static void emitted(void *context, const mi_event *event)
{
    fixture *f = context;
    if (event->kind == MI_RESET) ++f->resets;
}
static void reported(void *context, const ri_report *report)
{
    fixture *f = context;
    f->final_report = *report;
}
static int stop_requested(void *context)
{
    const fixture *f = context;
    if (f->transport_errors) return 1;
    if (f->stop_connections && f->resets >= f->stop_connections) return 1;
    return f->stop_attempts && f->attempt_n >= f->stop_attempts &&
        f->attempts[f->stop_attempts - 1u].closed;
}
static void begin_fixture(tb_config *c)
{
    memset(&mock, 0, sizeof(mock));
    cfg_defaults(c);
    c->qgc_enabled = 0; /* Synthetic packets must never reach a user's QGC. */
    (void)snprintf(c->serial_port, sizeof(c->serial_port), "auto");
    c->websocket_url[0] = 0;
    c->udp_enabled = 0;
    c->auto_scan = 1;
    c->serial_baud = DEFAULT_BAUD;
    c->probe_ms = 2;
    c->io_timeout_ms = 1;
    c->connect_timeout_ms = 1;
    c->reconnect_ms = 1;
    c->link_timeout_ms = 1000;
    c->duration_ms = 5000; /* Fail-safe only; callbacks end successful cases. */
    mock.idle_ms = c->probe_ms + 1u;
    mock.valid_baud = DEFAULT_BAUD;
    mock.stop_connections = 1;
    mock.packet_n = mi_heartbeat(mock.packet, sizeof(mock.packet), 42, 1, 0);
}
static int run_fixture(const tb_config *c)
{
    unsigned i;
    CHECK(mock.packet_n > 0);
    CHECK(ri_run(c, emitted, reported, stop_requested, &mock));
    CHECK(!mock.transport_errors);
    CHECK(!strcmp(mock.final_report.status, "STOPPED"));
    for (i = 0; i < mock.attempt_n; ++i)
        CHECK(!mock.attempts[i].opened || mock.attempts[i].closed);
    return 1;
}
static int expect_attempt(unsigned index, const char *port, unsigned baud)
{
    CHECK(index < mock.attempt_n);
    CHECK(!strcmp(mock.attempts[index].endpoint, port));
    CHECK(mock.attempts[index].baud == baud);
    return 1;
}

static int preferred_pass_then_fallback(void)
{
    tb_config c;
    begin_fixture(&c);
    mock.ports[0] = "COM11"; mock.ports[1] = "COM12"; mock.ports[2] = "COM13";
    mock.port_n = 3;
    mock.valid_port = "COM12"; mock.valid_baud = FALLBACK_BAUD;
    CHECK(run_fixture(&c));
    CHECK(mock.attempt_n == 5);
    for (unsigned i = 0; i < mock.port_n; ++i) CHECK(expect_attempt(i, mock.ports[i], DEFAULT_BAUD));
    CHECK(expect_attempt(3, "COM11", FALLBACK_BAUD));
    CHECK(expect_attempt(4, "COM12", FALLBACK_BAUD));
    CHECK(mock.resets == 1 && mock.final_report.connections == 1);
    CHECK(mock.final_report.total_frames == 1);
    CHECK(mock.final_report.serial_baud == FALLBACK_BAUD);
    CHECK(!strcmp(mock.final_report.source, "COM12"));
    CHECK(!mock.premature_writes);
    return 1;
}

static int supported_rates_once_per_pass(void)
{
    /* Independent expectations: do not derive these from the policy helper. */
    static const unsigned expected[] = {57600, 115200, 230400, 460800, 921600, 38400, 19200, 9600};
    tb_config c;
    unsigned rate, port;
    begin_fixture(&c);
    c.serial_baud = FALLBACK_BAUD;
    mock.ports[0] = "COM21"; mock.ports[1] = "COM22"; mock.port_n = 2;
    mock.stop_connections = 0;
    mock.stop_attempts = (unsigned)(sizeof(expected) / sizeof(expected[0])) * (unsigned)mock.port_n;
    CHECK(run_fixture(&c));
    CHECK(mock.attempt_n == mock.stop_attempts);
    for (rate = 0; rate < sizeof(expected) / sizeof(expected[0]); ++rate)
        for (port = 0; port < mock.port_n; ++port)
            CHECK(expect_attempt(rate * (unsigned)mock.port_n + port, mock.ports[port], expected[rate]));
    CHECK(!mock.resets && !mock.final_report.connections && !mock.premature_writes);
    return 1;
}

static int unavailable_ports_are_skipped(void)
{
    tb_config c;
    begin_fixture(&c);
    mock.ports[0] = mock.missing_port = "COM31";
    mock.ports[1] = mock.busy_port = "COM32";
    mock.ports[2] = mock.valid_port = "COM33";
    mock.port_n = 3;
    CHECK(run_fixture(&c));
    CHECK(mock.attempt_n == 3 && mock.resets == 1);
    for (unsigned i = 0; i < mock.port_n; ++i) CHECK(expect_attempt(i, mock.ports[i], DEFAULT_BAUD));
    CHECK(!mock.attempts[0].opened && !mock.attempts[1].opened);
    CHECK(!mock.attempts[0].reads && !mock.attempts[1].reads);
    CHECK(mock.final_report.serial_baud == DEFAULT_BAUD && !mock.premature_writes);
    return 1;
}

static int manual_mode_keeps_port_and_rate(void)
{
    tb_config c;
    begin_fixture(&c);
    c.auto_scan = 0;
    c.serial_baud = FIXED_BAUD;
    (void)snprintf(c.serial_port, sizeof(c.serial_port), "COM77");
    mock.ports[0] = mock.valid_port = "COM78"; mock.port_n = 1;
    mock.stop_connections = 0; mock.stop_attempts = 3;
    CHECK(run_fixture(&c));
    CHECK(!mock.enumerations && mock.attempt_n == mock.stop_attempts);
    for (unsigned i = 0; i < mock.attempt_n; ++i) CHECK(expect_attempt(i, "COM77", FIXED_BAUD));
    CHECK(!mock.resets && !mock.final_report.connections);
    return 1;
}

static int reconnect_refreshes_inventory_and_preferred_rate(void)
{
    tb_config c;
    begin_fixture(&c);
    mock.ports[0] = mock.valid_port = "COM41"; mock.port_n = 1;
    mock.valid_baud = FALLBACK_BAUD;
    mock.replacement[0] = mock.replacement_valid_port = "COM42"; mock.replacement_n = 1;
    mock.replacement_baud = DEFAULT_BAUD;
    mock.reconnect = 1; mock.stop_connections = 2;
    CHECK(run_fixture(&c));
    CHECK(mock.attempt_n == 3 && mock.enumerations >= 3);
    CHECK(expect_attempt(0, "COM41", DEFAULT_BAUD));
    CHECK(expect_attempt(1, "COM41", FALLBACK_BAUD));
    CHECK(expect_attempt(2, "COM42", DEFAULT_BAUD));
    CHECK(mock.resets == 2 && mock.final_report.connections == 2);
    CHECK(mock.final_report.total_frames == 2 && mock.final_report.mav.frames == 1);
    CHECK(mock.final_report.serial_baud == DEFAULT_BAUD);
    CHECK(!strcmp(mock.final_report.source, "COM42") && !mock.premature_writes);
    return 1;
}

static int noise_never_selects_or_announces(void)
{
    tb_config c;
    begin_fixture(&c);
    /* Explicit preference still requires validation when auto_scan is enabled. */
    (void)snprintf(c.serial_port, sizeof(c.serial_port), "COM51");
    c.probe_ms = 20; mock.idle_ms = c.probe_ms + 1u;
    mock.ports[0] = mock.noise_port = "COM51";
    mock.ports[1] = mock.bad_crc_port = "COM52";
    mock.ports[2] = mock.valid_port = "COM53"; mock.port_n = 3;
    CHECK(run_fixture(&c));
    CHECK(mock.attempt_n == 3);
    for (unsigned i = 0; i < mock.port_n; ++i) CHECK(expect_attempt(i, mock.ports[i], DEFAULT_BAUD));
    CHECK(mock.resets == 1 && mock.final_report.connections == 1);
    CHECK(mock.final_report.total_frames == 1 && !strcmp(mock.final_report.source, "COM53"));
    CHECK(!mock.attempts[0].writes && !mock.attempts[1].writes && !mock.premature_writes);
    CHECK(mock.attempts[2].writes > 0);
    return 1;
}

int main(void)
{
    if (!preferred_pass_then_fallback() || !supported_rates_once_per_pass() ||
        !unavailable_ports_are_skipped() || !manual_mode_keeps_port_and_rate() ||
        !reconnect_refreshes_inventory_and_preferred_rate() || !noise_never_selects_or_announces()) return 1;
    printf("serial discovery: %u checks passed\n", checks);
    return 0;
}
