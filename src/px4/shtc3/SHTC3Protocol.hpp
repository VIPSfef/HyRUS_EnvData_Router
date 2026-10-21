/****************************************************************************
 *
 *   Copyright (c) 2026 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

#pragma once

#include <cstddef>
#include <cstdint>

// SHTC3 datasheet v4 (December 2022). No PX4 dependencies: used unchanged by
// the real driver and fake-I2C tests. No clock stretching or blocking delays.
namespace shtc3
{
static constexpr uint8_t I2C_ADDRESS = 0x70;
static constexpr uint16_t WAKE = 0x3517;
static constexpr uint16_t SLEEP = 0xB098;
static constexpr uint16_t MEASURE = 0x7866; // normal power, temperature first, no clock stretching
static constexpr uint16_t READ_ID = 0xEFC8;
static constexpr uint16_t RESET = 0x805D;
static constexpr uint32_t WAKE_US = 300; // wake/reset maximum 240 us, rounded up
static constexpr uint32_t CONVERSION_US = 13000; // normal-power maximum 12.1 ms
static constexpr uint32_t PERIOD_US = 100000; // 10 Hz
static constexpr uint32_t MAX_LATE_US = 1000000;
static constexpr uint32_t MAX_RETRY_US = 1000000;
static constexpr uint32_t BASE_RETRY_US = 10000;

enum class Fault : uint8_t { None, Io, Crc, Id, Timing };
enum class State : uint8_t { Wake, WakeWait, Reset, ResetWait, ReadId, CollectId, Measure, Collect, Sleep };

struct Reading {
	uint64_t timestamp_sample{0}; // command-issued time before I2C write, approximate conversion start
	float temperature_c{0.f};
	float humidity_percent{0.f};
};

struct Counters {
	uint32_t samples{0};
	uint32_t io_errors{0};
	uint32_t crc_errors{0};
	uint32_t id_errors{0};
	uint32_t timing_errors{0};
	uint32_t resets{0};
	uint32_t errors{0};
};

struct Step {
	uint32_t delay_us{1};
	bool ready{false};
	Reading reading{};
	Fault fault{Fault::None};
};

class Core
{
public:
	explicit Core(uint32_t period_us = PERIOD_US) :
		_period_us(period_us >= CONVERSION_US + WAKE_US + 1000 ? period_us : PERIOD_US) {}

	void start(uint64_t now_us, bool already_awake = false)
	{
		_state = already_awake ? State::WakeWait : State::Wake;
		_deadline_us = already_awake ? add_time(now_us, WAKE_US) : now_us;
		_last_tick_us = now_us;
		_sample_start_us = 0;
		_next_cycle_us = already_awake ? add_time(now_us, _period_us) : 0;
		_started = true;
		_identified = _need_reset = false;
		_consecutive_errors = 0;
		_last_fault = Fault::None;
	}

	static uint8_t crc8(const uint8_t *data, size_t length)
	{
		uint8_t crc = 0xff;

		for (size_t i = 0; i < length; ++i) {
			crc ^= data[i];

			for (unsigned bit = 0; bit < 8; ++bit) {
				crc = static_cast<uint8_t>((crc & 0x80) ? (crc << 1) ^ 0x31 : crc << 1);
			}
		}

		return crc;
	}

	static bool valid_id(const uint8_t *data)
	{
		const uint16_t id = static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
		return crc8(data, 2) == data[2] && (id & 0x083f) == 0x0807;
	}

	static bool decode_measurement(const uint8_t *data, Reading &reading)
	{
		if (crc8(data, 2) != data[2] || crc8(data + 3, 2) != data[5]) {
			return false;
		}

		const uint16_t raw_temperature = static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
		const uint16_t raw_humidity = static_cast<uint16_t>((static_cast<uint16_t>(data[3]) << 8) | data[4]);
		reading.temperature_c = -45.f + 175.f * raw_temperature / 65536.f;
		reading.humidity_percent = 100.f * raw_humidity / 65536.f;
		return true;
	}

	State state() const { return _state; }
	Fault last_fault() const { return _last_fault; }
	const Counters &counters() const { return _counters; }
	uint64_t deadline_us() const { return _deadline_us; }

	/** Bus exposes bool write_command(uint16_t), bool read_response(uint8_t*,size_t).
	 * One transfer at most per tick. The caller schedules delay_us; it must not sleep
	 * in an I2C work queue. A ready sample passed both CRCs and a checked product ID. */
	template<typename Bus>
	Step tick(Bus &bus, uint64_t now_us)
	{
		if (!_started) { start(now_us); }

		if (now_us < _last_tick_us) {
			_last_tick_us = now_us;
			return fail(Fault::Timing, now_us);
		}

		_last_tick_us = now_us;

		// Saturating a measurement deadline could shorten the mandatory wait.
		// Reject an unrepresentable scheduling horizon before any I2C access.
		if (now_us > UINT64_MAX - _period_us || now_us > UINT64_MAX - MAX_RETRY_US) {
			return fail(Fault::Timing, now_us);
		}

		if (now_us < _deadline_us) {
			Step result;
			const uint64_t remaining = _deadline_us - now_us;
			result.delay_us = remaining > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(remaining);
			return result;
		}

		switch (_state) {
		case State::Wake:
			if (!bus.write_command(WAKE)) { return fail(Fault::Io, now_us); }

			_next_cycle_us = add_time(now_us, _period_us);
			_state = State::WakeWait;
			return next(now_us, WAKE_US);

		case State::WakeWait:
			_state = _need_reset ? State::Reset : (_identified ? State::Measure : State::ReadId);
			return next(now_us, 1);

		case State::Reset:
			if (!bus.write_command(RESET)) { return fail(Fault::Io, now_us); }

			increment(_counters.resets);
			_state = State::ResetWait;
			return next(now_us, WAKE_US);

		case State::ResetWait:
			_need_reset = false;
			_state = State::ReadId;
			return next(now_us, 1);

		case State::ReadId:
			if (!bus.write_command(READ_ID)) { return fail(Fault::Io, now_us); }

			_state = State::CollectId;
			return next(now_us, WAKE_US);

		case State::CollectId: {
				uint8_t data[3] {};

				if (!bus.read_response(data, sizeof(data))) { return fail(Fault::Io, now_us); }

				if (crc8(data, 2) != data[2]) { return fail(Fault::Crc, now_us); }

				if (!valid_id(data)) { return fail(Fault::Id, now_us); }

				_identified = true;
				_state = State::Measure;
				return next(now_us, 1);
			}

		case State::Measure:
			if (!bus.write_command(MEASURE)) { return fail(Fault::Io, now_us); }

			_sample_start_us = now_us;
			_state = State::Collect;
			return next(now_us, CONVERSION_US);

		case State::Collect: {
				if (now_us - _sample_start_us > MAX_LATE_US) { return fail(Fault::Timing, now_us); }

				uint8_t data[6] {};
				Reading reading;

				if (!bus.read_response(data, sizeof(data))) { return fail(Fault::Io, now_us); }

				if (!decode_measurement(data, reading)) { return fail(Fault::Crc, now_us); }

				reading.timestamp_sample = _sample_start_us;
				increment(_counters.samples);
				_consecutive_errors = 0;
				_last_fault = Fault::None;
				_state = State::Sleep;
				Step result = next(now_us, 1);
				result.ready = true;
				result.reading = reading;
				return result;
			}

		case State::Sleep:
			if (!bus.write_command(SLEEP)) { return fail(Fault::Io, now_us); }

			_state = State::Wake;
			return next(now_us, _next_cycle_us > now_us ? static_cast<uint32_t>(_next_cycle_us - now_us) : 1);
		}

		return fail(Fault::Timing, now_us);
	}

private:
	static void increment(uint32_t &counter) { if (counter != UINT32_MAX) { ++counter; } }
	static uint64_t add_time(uint64_t now, uint32_t delay) { return now > UINT64_MAX - delay ? UINT64_MAX : now + delay; }

	Step next(uint64_t now_us, uint32_t delay_us)
	{
		Step result;
		result.delay_us = delay_us ? delay_us : 1;
		_deadline_us = add_time(now_us, result.delay_us);
		return result;
	}

	Step fail(Fault fault, uint64_t now_us)
	{
		increment(_counters.errors);

		switch (fault) {
		case Fault::Io: increment(_counters.io_errors); break;

		case Fault::Crc: increment(_counters.crc_errors); break;

		case Fault::Id: increment(_counters.id_errors); break;

		case Fault::Timing: increment(_counters.timing_errors); break;

		case Fault::None: break;
		}

		if (_consecutive_errors < 8) { ++_consecutive_errors; }

		uint32_t delay_us = BASE_RETRY_US << (_consecutive_errors - 1);

		if (delay_us > MAX_RETRY_US) { delay_us = MAX_RETRY_US; }

		_state = State::Wake;
		_identified = false;
		_need_reset = true;
		_last_fault = fault;
		Step result = next(now_us, delay_us);
		result.fault = fault;
		return result;
	}

	Counters _counters{};
	uint64_t _deadline_us{0};
	uint64_t _last_tick_us{0};
	uint64_t _sample_start_us{0};
	uint64_t _next_cycle_us{0};
	uint32_t _period_us;
	State _state{State::Wake};
	Fault _last_fault{Fault::None};
	uint8_t _consecutive_errors{0};
	bool _started{false};
	bool _identified{false};
	bool _need_reset{false};
};
} // namespace shtc3
