// SPDX-License-Identifier: BSD-3-Clause
// Compile and execute the production stream header; stub only its platform IO.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <initializer_list>
#include <memory>
#include <uORB/Subscription.hpp>

namespace fake
{
Topic topics[2] {};
unsigned selected_instance = 99;
unsigned selected_topic = 0;
unsigned update_calls = 0;
unsigned advertised_calls = 0;
uint64_t now_us = 2000000;
uint8_t proto_version = 2;
unsigned sent_count = 0;
unsigned sent_channel = 99;
}

constexpr uint16_t MAVLINK_MSG_ID_SENS_ATMOS = 8009;
constexpr unsigned MAVLINK_MSG_ID_SENS_ATMOS_LEN = 16;
constexpr unsigned MAVLINK_NUM_NON_PAYLOAD_BYTES = 12;
#define PX4_ISFINITE(value) std::isfinite(value)
struct mavlink_sens_atmos_t {
	uint64_t timestamp{0};
	float TempAmbient{0.f};
	float Humidity{0.f};
};
static mavlink_sens_atmos_t sent_message{};
inline uint8_t mavlink_get_proto_version(uint8_t) { return fake::proto_version; }
inline void mavlink_msg_sens_atmos_send_struct(uint8_t channel, const mavlink_sens_atmos_t *message)
{
	++fake::sent_count;
	fake::sent_channel = channel;
	sent_message = *message;
}
class Mavlink
{
public:
	uint8_t get_channel() const { return 3; }
};
class MavlinkStream
{
public:
	explicit MavlinkStream(Mavlink *mavlink) : _mavlink(mavlink) {}
	virtual ~MavlinkStream() = default;
	virtual const char *get_name() const = 0;
	virtual uint16_t get_id() = 0;
	virtual unsigned get_size() = 0;
	bool tick() { return send(); } // Exercise the private override by virtual dispatch.
protected:
	virtual bool send() = 0;
	Mavlink *_mavlink;
};
#include "SENS_ATMOS.hpp"

static unsigned checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
			std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); std::exit(1); } } while (0)

static void reset()
{
	fake::topics[0] = {};
	fake::topics[1] = {};
	fake::update_calls = fake::advertised_calls = fake::sent_count = 0;
	fake::sent_channel = 99;
	fake::proto_version = 2;
	fake::now_us = 2000000;
	sent_message = {};
}
static sensor_hygrometer_s sample(float temperature = 22.25f, float humidity = 47.5f)
{
	sensor_hygrometer_s s{};
	s.timestamp = 1999500; // A distinct publication clock must never replace acquisition time.
	s.timestamp_sample = 1986000;
	s.device_id = 0x12345678;
	s.temperature = temperature;
	s.humidity = humidity;
	return s;
}
static void publish(const sensor_hygrometer_s &s, unsigned instance = 0)
{
	fake::topics[instance].sample = s;
	fake::topics[instance].advertised = true;
	fake::topics[instance].unread = true;
}
static void rejected(MavlinkStream &stream, const sensor_hygrometer_s &s)
{
	reset();
	publish(s);
	CHECK(stream.get_size() == 28);
	CHECK(!stream.tick());
	CHECK(fake::update_calls == 1 && !fake::topics[0].unread);
	CHECK(fake::sent_count == 0);
}
int main()
{
	Mavlink mavlink;
	std::unique_ptr<MavlinkStream> stream(MavlinkStreamSensAtmos::new_instance(&mavlink));
	CHECK(fake::selected_topic == 42 && fake::selected_instance == 0);
	CHECK(MavlinkStreamSensAtmos::get_id_static() == 8009 && stream->get_id() == 8009);
	CHECK(std::strcmp(stream->get_name(), "SENS_ATMOS") == 0);

	reset();
	CHECK(stream->get_size() == 0 && !stream->tick());
	CHECK(fake::sent_count == 0);
	// Instance one is neither subscribed to nor accidentally forwarded.
	publish(sample(-10.f, 90.f), 1);
	CHECK(stream->get_size() == 0 && !stream->tick());
	CHECK(fake::topics[1].unread && fake::sent_count == 0);

	reset();
	publish(sample());
	fake::proto_version = 1;
	CHECK(stream->get_size() == 0);
	CHECK(fake::advertised_calls == 0);
	CHECK(!stream->tick());
	CHECK(fake::update_calls == 0 && fake::topics[0].unread && fake::sent_count == 0);
	fake::proto_version = 2;
	CHECK(stream->get_size() == 28 && fake::topics[0].unread);
	CHECK(stream->tick());
	CHECK(fake::sent_count == 1 && fake::sent_channel == 3 && fake::update_calls == 1);
	CHECK(sent_message.timestamp == sample().timestamp_sample);
	CHECK(sent_message.timestamp != sample().timestamp);
	CHECK(sent_message.TempAmbient == 22.25f && sent_message.Humidity == 47.5f);
	CHECK(!stream->tick() && fake::sent_count == 1); // No resending a stale subscription value.

	// Signed wire values remain degC and percent here. SI conversion belongs to
	// the collector, not the PX4 stream: -10 C and 50% must not become 263K/.5.
	reset();
	publish(sample(-10.f, 50.f));
	CHECK(stream->tick());
	CHECK(sent_message.TempAmbient == -10.f && sent_message.Humidity == 50.f);
	CHECK(sent_message.timestamp == 1986000);

	// Age boundary inclusive at 1 second; zero, future and older are rejected.
	auto s = sample();
	s.timestamp_sample = 1000000;
	reset(); publish(s);
	CHECK(stream->tick() && sent_message.timestamp == 1000000);
	s.timestamp_sample = 999999; rejected(*stream, s);
	s.timestamp_sample = 2000001; rejected(*stream, s);
	s.timestamp_sample = 0; rejected(*stream, s);
	s.timestamp_sample = UINT64_MAX; rejected(*stream, s);
	s.timestamp_sample = 2000000;
	reset(); publish(s);
	CHECK(stream->tick() && sent_message.timestamp == 2000000);

	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float inf = std::numeric_limits<float>::infinity();

	for (float t : {nan, inf, -inf, -273.16f}) { rejected(*stream, sample(t, 50.f)); }

	for (float h : {nan, inf, -inf, -0.01f, 100.01f}) { rejected(*stream, sample(20.f, h)); }

	for (float h : {0.f, 100.f}) {
		reset(); publish(sample(-273.15f, h));
		CHECK(stream->tick());
		CHECK(sent_message.TempAmbient == -273.15f && sent_message.Humidity == h);
	}
	std::printf("SENS_ATMOS_STREAM PASS checks=%u actual_header=1 MAV1_consumption=0 hardware_io=0\n", checks);
	return 0;
}
