/* Native verification of the exact PX4 SHTC3 portable production core.
 * The fake bus supplies bytes; it contains no second state machine.
 * All fixtures and observations stay in process memory. No hardware claim. */
#include "../SHTC3Protocol.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <initializer_list>

static unsigned long long checks;
static void require(bool value, const char *condition, int line)
{
    ++checks;
    if (!value) {
        std::fprintf(stderr, "SHTC3 FAIL line %d: %s\n", line, condition);
        std::exit(1);
    }
}
#define CHECK(condition) require((condition), #condition, __LINE__)

/* Independent polynomial long division, not the production shift-register
 * byte loop. Sensirion v4 Table16 supplies the BEEF->92 cross-check. */
static uint8_t reference_crc_word(uint16_t word)
{
    uint32_t polynomial = (UINT32_C(0xff) << 16) ^ (static_cast<uint32_t>(word) << 8);
    for (int bit = 23; bit >= 8; --bit) {
        if (polynomial & (UINT32_C(1) << bit)) {
            polynomial ^= UINT32_C(0x131) << (bit - 8);
        }
    }
    return static_cast<uint8_t>(polynomial);
}
static void put_word(uint8_t *bytes, uint16_t value)
{
    bytes[0] = static_cast<uint8_t>(value >> 8);
    bytes[1] = static_cast<uint8_t>(value);
    bytes[2] = reference_crc_word(value);
}

struct Event {
    uint64_t time;
    uint16_t command;
    uint8_t length;
    char kind;
    bool success;
};
struct FakeBus {
    uint64_t now{0};
    Event events[1024]{};
    unsigned count{0};
    bool present{true};
    uint16_t failing_command{0};
    size_t failing_read_length{0};
    uint8_t id[3]{};
    uint8_t measurement[6]{0x64, 0x8b, 0xc7, 0xa1, 0x33, 0x1c};

    FakeBus() { put_word(id, 0x0807); }
    void record(char kind, uint16_t command, size_t length, bool success)
    {
        CHECK(count < sizeof(events) / sizeof(events[0]));
        CHECK(length <= 6);
        events[count++] = Event{now, command, static_cast<uint8_t>(length), kind, success};
    }
    bool write_command(uint16_t command)
    {
        const bool success = present && command != failing_command;
        record('W', command, 2, success);
        return success;
    }
    bool read_response(uint8_t *data, size_t length)
    {
        CHECK(length == 3 || length == 6);
        const bool success = present && length != failing_read_length;
        record('R', 0, length, success);
        if (success) { std::memcpy(data, length == 3 ? id : measurement, length); }
        return success;
    }
    unsigned writes(uint16_t command) const
    {
        unsigned n = 0;
        for (unsigned i = 0; i < count; ++i) {
            if (events[i].kind == 'W' && events[i].command == command) { ++n; }
        }
        return n;
    }
    unsigned reads(size_t length) const
    {
        unsigned n = 0;
        for (unsigned i = 0; i < count; ++i) {
            if (events[i].kind == 'R' && events[i].length == length) { ++n; }
        }
        return n;
    }
};
struct Harness {
    shtc3::Core core;
    FakeBus bus;
    uint64_t now;
    explicit Harness(uint64_t start = 1000000, uint32_t period = shtc3::PERIOD_US) : core(period), now(start)
    {
        core.start(start);
    }
    shtc3::Step at(uint64_t when)
    {
        now = bus.now = when;
        const unsigned before = bus.count;
        const shtc3::Step result = core.tick(bus, when);
        CHECK(bus.count - before <= 1); // never a transfer loop in one work item
        CHECK(result.delay_us > 0); // a caller can schedule, not spin/sleep
        if (result.ready) {
            CHECK(result.fault == shtc3::Fault::None);
            CHECK(result.reading.timestamp_sample <= when);
            CHECK(when - result.reading.timestamp_sample >= shtc3::CONVERSION_US);
        }
        return result;
    }
    shtc3::Step next() { return at(core.deadline_us()); }
    shtc3::Step ready()
    {
        for (unsigned i = 0; i < 64; ++i) {
            shtc3::Step result = next();
            if (result.ready) { return result; }
        }
        CHECK(false); return shtc3::Step{};
    }
    shtc3::Step fault()
    {
        for (unsigned i = 0; i < 64; ++i) {
            shtc3::Step result = next();
            CHECK(!result.ready);
            if (result.fault != shtc3::Fault::None) { return result; }
        }
        CHECK(false); return shtc3::Step{};
    }
};

static void protocol_vectors()
{
    CHECK(shtc3::I2C_ADDRESS == 0x70);
    CHECK(shtc3::WAKE == 0x3517 && shtc3::SLEEP == 0xb098);
    CHECK(shtc3::READ_ID == 0xefc8 && shtc3::RESET == 0x805d);
    CHECK(shtc3::MEASURE == 0x7866); // normal, temperature first, no stretching
    CHECK(shtc3::WAKE_US >= 240 && shtc3::CONVERSION_US >= 12100);
    const uint8_t zero[] = {0}, beef[] = {0xbe, 0xef}, zero_word[] = {0, 0};
    CHECK(shtc3::Core::crc8(zero, 1) == 0xac);
    CHECK(shtc3::Core::crc8(beef, 2) == 0x92);
    CHECK(shtc3::Core::crc8(zero_word, 2) == 0x81); // distinguish one vs two bytes
    CHECK(shtc3::Core::crc8(nullptr, 0) == 0xff);

    for (uint32_t word = 0; word <= 65535; ++word) {
        uint8_t bytes[3]; put_word(bytes, static_cast<uint16_t>(word));
        CHECK(shtc3::Core::crc8(bytes, 2) == reference_crc_word(static_cast<uint16_t>(word)));
        CHECK(shtc3::Core::valid_id(bytes) == ((word & 0x083f) == 0x0807));
        bytes[2] ^= 0x80;
        CHECK(!shtc3::Core::valid_id(bytes));
    }
    const uint8_t example[] = {0x64, 0x8b, 0xc7, 0xa1, 0x33, 0x1c};
    shtc3::Reading value;
    CHECK(shtc3::Core::decode_measurement(example, value));
    CHECK(std::fabs(value.temperature_c - 23.7305450439453125) < 0.00002);
    CHECK(std::fabs(value.humidity_percent - 62.96844482421875) < 0.00001);
    // Figure7 rounds these values to23.7C and63%RH; reorder its RH-first bytes.
    for (unsigned byte = 0; byte < 6; ++byte) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            uint8_t damaged[6]; std::memcpy(damaged, example, 6);
            damaged[byte] ^= static_cast<uint8_t>(1u << bit);
            CHECK(!shtc3::Core::decode_measurement(damaged, value));
        }
    }
    double previous_temperature = -100;
    for (uint32_t word = 0; word <= 65535; ++word) {
        uint8_t bytes[6]; put_word(bytes, static_cast<uint16_t>(word));
        put_word(bytes + 3, static_cast<uint16_t>(65535 - word));
        CHECK(shtc3::Core::decode_measurement(bytes, value));
        const double expected_temperature = -45.0 + 175.0 * word / 65536.0;
        const double expected_humidity = 100.0 * (65535 - word) / 65536.0;
        CHECK(std::fabs(value.temperature_c - expected_temperature) <= 0.00002);
        CHECK(std::fabs(value.humidity_percent - expected_humidity) <= 0.00001);
        CHECK(value.temperature_c > previous_temperature);
        CHECK(value.humidity_percent >= 0 && value.humidity_percent <= 100);
        previous_temperature = value.temperature_c;
    }
    std::puts("protocol: official address/commands, exhaustive CRC/ID/conversions, both CRCs: PASS");
}

static void cycle_and_deadlines()
{
    Harness h;
    auto result = h.next();
    CHECK(h.bus.count == 1 && h.bus.events[0].command == 0x3517);
    CHECK(h.core.deadline_us() - h.now >= 240);
    const uint64_t wake_deadline = h.core.deadline_us();
    for (unsigned i = 0; i < 1000; ++i) {
        result = h.at(wake_deadline - 1);
        CHECK(!result.ready && h.bus.count == 1 && result.delay_us == 1);
    }
    while (h.core.state() != shtc3::State::Collect) { h.next(); }
    const unsigned before = h.bus.count;
    const uint64_t conversion_deadline = h.core.deadline_us();
    CHECK(h.bus.events[before - 1].command == 0x7866);
    CHECK(conversion_deadline - h.now >= 12100);
    result = h.at(conversion_deadline - 1);
    CHECK(!result.ready && h.bus.count == before);
    result = h.at(conversion_deadline);
    CHECK(result.ready && result.reading.timestamp_sample == h.bus.events[before - 1].time);
    CHECK(std::fabs(result.reading.temperature_c - 23.7305450439453125) < 0.00002);
    h.next(); // sleep follows the valid read, no blocking wait inside a tick
    CHECK(h.bus.count == 6);
    const char kind[] = {'W', 'W', 'R', 'W', 'R', 'W'};
    const uint16_t command[] = {0x3517, 0xefc8, 0, 0x7866, 0, 0xb098};
    const unsigned length[] = {2, 2, 3, 2, 6, 2};
    for (unsigned i = 0; i < 6; ++i) {
        CHECK(h.bus.events[i].kind == kind[i] && h.bus.events[i].command == command[i]);
        CHECK(h.bus.events[i].length == length[i]);
    }
    const uint64_t first_timestamp = result.reading.timestamp_sample;
    result = h.ready(); h.next();
    CHECK(h.bus.writes(shtc3::READ_ID) == 1 && h.bus.reads(3) == 1);
    CHECK(h.bus.writes(shtc3::WAKE) == 2 && h.bus.writes(shtc3::SLEEP) == 2);
    // The first probe also reads ID. Later wake/convert cycles keep the
    // configured phase rather than adding the wake delay to every period.
    CHECK(result.reading.timestamp_sample - first_timestamp >= shtc3::PERIOD_US - shtc3::WAKE_US - 2);
    CHECK(result.reading.timestamp_sample - first_timestamp <= shtc3::PERIOD_US);
    CHECK(h.core.counters().samples == 2 && h.core.counters().errors == 0);
    const uint64_t second_timestamp = result.reading.timestamp_sample;
    result = h.ready(); h.next();
    CHECK(result.reading.timestamp_sample - second_timestamp == shtc3::PERIOD_US);
    CHECK(h.core.counters().samples == 3);
    std::puts("cycle: actual state core, byte order, nonblocking deadlines, conversion-start timestamp: PASS");
}

static void successful_probe_path()
{
    Harness h;
    h.bus.now = h.now;
    CHECK(h.bus.write_command(shtc3::WAKE)); // the actual PX4 presence probe
    h.core.start(h.now, true);
    CHECK(h.core.state() == shtc3::State::WakeWait);
    const uint64_t first_deadline = h.core.deadline_us();
    CHECK(first_deadline - h.now >= 240);
    CHECK(!h.at(first_deadline - 1).ready && h.bus.count == 1);
    CHECK(h.ready().ready);
    CHECK(h.bus.writes(shtc3::WAKE) == 1); // no duplicate wake in core.init path
    CHECK(h.bus.writes(shtc3::READ_ID) == 1 && h.bus.reads(3) == 1);
    CHECK(h.core.counters().samples == 1 && h.core.counters().errors == 0);
    std::puts("probe: actual already-awake initialization still waits and validates ID before publish: PASS");
}

static void crc_faults_and_recovery()
{
    for (unsigned crc_byte : {2u, 5u}) {
        Harness h;
        h.bus.measurement[crc_byte] ^= 1;
        CHECK(h.fault().fault == shtc3::Fault::Crc);
        CHECK(h.core.counters().samples == 0 && h.core.counters().crc_errors == 1);
        CHECK(h.core.counters().errors == 1 && h.bus.writes(shtc3::RESET) == 0);
        const unsigned failed_reads = h.bus.reads(6);
        h.bus.measurement[crc_byte] ^= 1;
        const auto value = h.ready();
        CHECK(value.ready && h.core.counters().samples == 1);
        CHECK(h.core.counters().resets == 1 && h.bus.writes(shtc3::RESET) == 1);
        CHECK(h.bus.writes(shtc3::READ_ID) == 2 && h.bus.reads(6) == failed_reads + 1);
        h.next(); // successful sleep
        h.bus.present = false;
        CHECK(h.fault().delay_us == shtc3::BASE_RETRY_US); // success resets retry escalation
    }
    Harness id_crc;
    id_crc.bus.id[2] ^= 1;
    CHECK(id_crc.fault().fault == shtc3::Fault::Crc);
    CHECK(id_crc.core.counters().samples == 0 && id_crc.bus.writes(shtc3::MEASURE) == 0);
    Harness wrong_id;
    put_word(wrong_id.bus.id, 0x0007); // valid CRC, missing the required bit11
    CHECK(wrong_id.fault().fault == shtc3::Fault::Id);
    CHECK(wrong_id.core.counters().id_errors == 1 && wrong_id.core.counters().samples == 0);
    CHECK(wrong_id.bus.writes(shtc3::MEASURE) == 0);
    std::puts("faults: either data CRC and ID rejection, reset/probe recovery, no fabricated publication: PASS");
}

static void absence_and_io_backoff()
{
    Harness absent;
    absent.bus.present = false;
    const uint32_t delays[] = {10000, 20000, 40000, 80000, 160000, 320000, 640000, 1000000, 1000000};
    for (uint32_t expected : delays) {
        const auto result = absent.fault();
        CHECK(result.fault == shtc3::Fault::Io && result.delay_us == expected);
        const unsigned before = absent.bus.count;
        const uint64_t deadline = absent.core.deadline_us();
        for (unsigned i = 0; i < 1000; ++i) {
            CHECK(!absent.at(deadline - 1).ready);
            CHECK(absent.bus.count == before);
        }
    }
    CHECK(absent.core.counters().samples == 0 && absent.bus.writes(shtc3::MEASURE) == 0);
    CHECK(absent.bus.count == sizeof(delays) / sizeof(delays[0]));
    for (uint16_t command : {shtc3::READ_ID, shtc3::MEASURE}) {
        Harness fail;
        fail.bus.failing_command = command;
        CHECK(fail.fault().fault == shtc3::Fault::Io);
        CHECK(fail.core.counters().samples == 0 && fail.bus.reads(6) == 0);
    }
    for (size_t length : {size_t(3), size_t(6)}) {
        Harness read_fail;
        read_fail.bus.failing_read_length = length;
        CHECK(read_fail.fault().fault == shtc3::Fault::Io);
        CHECK(read_fail.core.counters().samples == 0);
    }
    Harness reset_fail;
    reset_fail.bus.measurement[2] ^= 1;
    CHECK(reset_fail.fault().fault == shtc3::Fault::Crc);
    reset_fail.bus.failing_command = shtc3::RESET;
    CHECK(reset_fail.fault().fault == shtc3::Fault::Io);
    CHECK(reset_fail.core.counters().samples == 0);
    Harness sleep_fail;
    sleep_fail.bus.failing_command = shtc3::SLEEP;
    const auto valid = sleep_fail.ready();
    CHECK(sleep_fail.fault().fault == shtc3::Fault::Io);
    CHECK(sleep_fail.core.counters().samples == 1); // prior read passed both CRCs
    sleep_fail.bus.failing_command = 0;
    CHECK(sleep_fail.ready().reading.timestamp_sample > valid.reading.timestamp_sample);
    std::puts("absent/I2C: no fake data, no early polling, exponential10ms..1s bounded backoff: PASS");
}

static void timing_and_overflow()
{
    Harness late;
    while (late.core.state() != shtc3::State::Collect) { late.next(); }
    const unsigned before = late.bus.count;
    const uint64_t start = late.bus.events[before - 1].time;
    CHECK(late.at(start + shtc3::MAX_LATE_US + 1).fault == shtc3::Fault::Timing);
    CHECK(late.bus.count == before && late.core.counters().samples == 0);
    CHECK(late.core.counters().timing_errors == 1);
    CHECK(late.ready().ready); // recovery, not a permanently wedged state

    Harness backwards;
    while (backwards.core.state() != shtc3::State::Collect) { backwards.next(); }
    CHECK(backwards.at(backwards.now - 1).fault == shtc3::Fault::Timing);
    CHECK(backwards.core.counters().samples == 0 && backwards.core.counters().timing_errors == 1);
    CHECK(backwards.ready().ready);

    Harness wrap(UINT64_MAX - 500);
    CHECK(wrap.next().fault == shtc3::Fault::Timing);
    CHECK(wrap.at(UINT64_MAX).fault == shtc3::Fault::Timing);
    CHECK(wrap.bus.count == 0 && wrap.core.counters().samples == 0);
    CHECK(wrap.at(50).fault == shtc3::Fault::Timing);
    CHECK(wrap.bus.count == 0);
    CHECK(wrap.ready().ready);
    CHECK(wrap.core.counters().timing_errors >= 3);
    std::puts("timing: early/future collect blocked, late/regressing/wrapped clock drops and recovers: PASS");
}

static void counter_saturation()
{
    Harness h;
    // The underlying object is mutable. Seed boundary fixtures through its public
    // const view, avoiding production-only testing hooks or billions of retries.
    auto &counters = const_cast<shtc3::Counters &>(h.core.counters());
    counters.errors = counters.io_errors = UINT32_MAX - 1;
    h.bus.present = false;
    CHECK(h.fault().fault == shtc3::Fault::Io);
    CHECK(h.fault().fault == shtc3::Fault::Io);
    CHECK(counters.errors == UINT32_MAX && counters.io_errors == UINT32_MAX);
    h.bus.present = true;
    counters.samples = counters.resets = UINT32_MAX - 1;
    CHECK(h.ready().ready); h.next();
    CHECK(h.ready().ready); h.next();
    CHECK(counters.samples == UINT32_MAX && counters.resets == UINT32_MAX);
    h.bus.present = false;
    CHECK(h.fault().fault == shtc3::Fault::Io);
    h.bus.present = true;
    CHECK(h.ready().ready); // another reset increment while already UINT32_MAX
    CHECK(counters.samples == UINT32_MAX && counters.resets == UINT32_MAX);
    CHECK(counters.errors == UINT32_MAX && counters.io_errors == UINT32_MAX);
    std::puts("counters: error/sample/reset values saturate rather than wrap: PASS");
}

int main()
{
    const auto started = std::chrono::steady_clock::now();
    protocol_vectors();
    cycle_and_deadlines();
    successful_probe_path();
    crc_faults_and_recovery();
    absence_and_io_backoff();
    timing_and_overflow();
    counter_saturation();
    const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    std::printf("SHTC3_CORE_TEST_PASS checks=%llu core_bytes=%zu elapsed_ms=%.3f (native fake I2C; no hardware)\n",
                checks, sizeof(shtc3::Core), elapsed);
    return 0;
}
