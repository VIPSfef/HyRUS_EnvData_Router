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

#include <drivers/device/i2c.h>
#include <drivers/drv_hrt.h>
#include <drivers/drv_sensor.h>
#include <px4_platform_common/atomic.h>
#include <px4_platform_common/i2c_spi_buses.h>
#include <uORB/PublicationMulti.hpp>
#include <uORB/topics/sensor_hygrometer.h>
#include <lib/perf/perf_counter.h>

#include "SHTC3Protocol.hpp"

class SHTC3 : public device::I2C, public I2CSPIDriver<SHTC3>
{
public:
	explicit SHTC3(const I2CSPIDriverConfig &config);
	~SHTC3() override;

	static void print_usage();
	int init() override;
	void RunImpl();
	void print_status() override;

private:
	friend class shtc3::Core;
	int probe() override;
	bool write_command(uint16_t command);
	bool read_response(uint8_t *data, size_t length);
	void exit_and_cleanup() override;

	shtc3::Core _core{};
	uORB::PublicationMulti<sensor_hygrometer_s> _sensor_hygrometer_pub{ORB_ID(sensor_hygrometer)};
	perf_counter_t _cycle_perf{perf_alloc(PC_ELAPSED, MODULE_NAME ": cycle")};
	// Status reads these lock-free snapshots rather than racing the work-queue core.
	px4::atomic<uint32_t> _samples{0};
	px4::atomic<uint32_t> _io_errors{0};
	px4::atomic<uint32_t> _crc_errors{0};
	px4::atomic<uint32_t> _id_errors{0};
	px4::atomic<uint32_t> _timing_errors{0};
	px4::atomic<uint32_t> _resets{0};
	px4::atomic<uint32_t> _state{0};
	px4::atomic<uint32_t> _fault{0};
	px4::atomic<int> _publication_instance{-1};
};
