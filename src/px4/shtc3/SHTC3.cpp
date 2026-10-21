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

#include "SHTC3.hpp"

#include <inttypes.h>
#include <cstring>
#include <px4_platform_common/module.h>

extern "C" __EXPORT int shtc3_main(int argc, char *argv[]);

SHTC3::SHTC3(const I2CSPIDriverConfig &config) :
	I2C(config), I2CSPIDriver(config)
{
	_retries = 0; // Recovery is scheduled by the protocol core, never a busy retry loop.
	set_device_type(DRV_HYGRO_DEVTYPE_SHTC3);
}

SHTC3::~SHTC3()
{
	ScheduleClear();
	perf_free(_cycle_perf);
}

bool SHTC3::write_command(uint16_t command)
{
	const uint8_t data[2] {static_cast<uint8_t>(command >> 8), static_cast<uint8_t>(command)};
	return transfer(data, sizeof(data), nullptr, 0) == PX4_OK;
}

bool SHTC3::read_response(uint8_t *data, size_t length)
{
	return transfer(nullptr, 0, data, static_cast<unsigned>(length)) == PX4_OK;
}

int SHTC3::probe()
{
	// Presence probe only. Product ID and its CRC are validated asynchronously
	// after the mandatory wake delay, before any measurement can be published.
	return write_command(shtc3::WAKE) ? PX4_OK : PX4_ERROR;
}

int SHTC3::init()
{
	const int ret = I2C::init();

	if (ret != PX4_OK) {
		return ret;
	}

	_core.start(hrt_absolute_time(), true); // probe() already issued WAKE
	ScheduleDelayed(shtc3::WAKE_US);
	return PX4_OK;
}

void SHTC3::RunImpl()
{
	if (should_exit()) { return; }

	perf_begin(_cycle_perf);
	const shtc3::Step step = _core.tick(*this, hrt_absolute_time());

	if (step.ready) {
		sensor_hygrometer_s msg{};
		msg.timestamp_sample = step.reading.timestamp_sample;
		msg.timestamp = hrt_absolute_time();
		msg.device_id = get_device_id(); // bus identity, not the product ID or a serial number
		msg.temperature = step.reading.temperature_c;
		msg.humidity = step.reading.humidity_percent;
		_sensor_hygrometer_pub.publish(msg);
		_publication_instance.store(_sensor_hygrometer_pub.get_instance());
	}

	const auto &counts = _core.counters();
	_samples.store(counts.samples);
	_io_errors.store(counts.io_errors);
	_crc_errors.store(counts.crc_errors);
	_id_errors.store(counts.id_errors);
	_timing_errors.store(counts.timing_errors);
	_resets.store(counts.resets);
	_state.store(static_cast<uint32_t>(_core.state()));
	_fault.store(static_cast<uint32_t>(_core.last_fault()));
	perf_end(_cycle_perf);
	ScheduleDelayed(step.delay_us); // no conversion sleep blocks the shared I2C queue
}

void SHTC3::exit_and_cleanup()
{
	(void)write_command(shtc3::SLEEP);
	I2CSPIDriverBase::exit_and_cleanup();
}

void SHTC3::print_status()
{
	I2CSPIDriverBase::print_status();
	PX4_INFO("samples: %" PRIu32 ", uORB instance: %d", _samples.load(), _publication_instance.load());
	PX4_INFO("errors I/O:%" PRIu32 " CRC:%" PRIu32 " ID:%" PRIu32 " timing:%" PRIu32,
		 _io_errors.load(), _crc_errors.load(), _id_errors.load(), _timing_errors.load());
	PX4_INFO("resets:%" PRIu32 " state:%" PRIu32 " fault:%" PRIu32,
		 _resets.load(), _state.load(), _fault.load());
	perf_print_counter(_cycle_perf);
}

void SHTC3::print_usage()
{
	PRINT_MODULE_DESCRIPTION(
		R"DESCR_STR(
### Description
Sensirion SHTC3 at fixed I2C address 0x70. Normal-power, no-clock-stretch
conversion runs at 10 Hz on the shared I2C work queue without blocking sleeps.
Both measurement CRCs and product ID are checked before sensor_hygrometer publication.
Timestamp_sample is command-issued time, an approximation of conversion start;
it does not identify an exact physical sensing instant or bound sensor response lag.
### Examples
Start on the external I2C bus:
$ shtc3 start -X
Show sample/error counters:
$ shtc3 status
)DESCR_STR");
	PRINT_MODULE_USAGE_NAME("shtc3", "driver");
	PRINT_MODULE_USAGE_COMMAND("start");
	PRINT_MODULE_USAGE_PARAMS_I2C_SPI_DRIVER(true, false);
	PRINT_MODULE_USAGE_DEFAULT_COMMANDS();
}

int shtc3_main(int argc, char *argv[])
{
	BusCLIArguments cli{true, false};
	cli.default_i2c_frequency = 400000;
	cli.i2c_address = shtc3::I2C_ADDRESS;
	const char *verb = cli.parseDefaultArguments(argc, argv);

	if (!verb) { SHTC3::print_usage(); return PX4_ERROR; }

	if (cli.i2c_address != shtc3::I2C_ADDRESS) {
		PX4_ERR("SHTC3 address is fixed at 0x70");
		return PX4_ERROR;
	}

	BusInstanceIterator iterator(MODULE_NAME, cli, DRV_HYGRO_DEVTYPE_SHTC3);

	if (!strcmp(verb, "start")) { return SHTC3::module_start(cli, iterator); }

	if (!strcmp(verb, "stop")) { return SHTC3::module_stop(iterator); }

	if (!strcmp(verb, "status")) { return SHTC3::module_status(iterator); }

	SHTC3::print_usage();
	return PX4_ERROR;
}
