// SPDX-License-Identifier: BSD-3-Clause
// Maintenance-only, one-shot SHTC3 identification. No scan, reset or publishing.
#include <px4_platform_common/px4_config.h>
#include <px4_platform_common/atomic.h>
#include <px4_platform_common/log.h>
#include <px4_platform_common/module.h>
#include <px4_platform_common/time.h>
#include <drivers/drv_hrt.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <inttypes.h>

#include "SHTC3Probe.hpp"

namespace
{
constexpr uint32_t FREQUENCY_HZ = 100000;
px4::atomic_bool probe_running{false};

class ProbeGuard
{
public:
	~ProbeGuard() { probe_running.store(false); }
};

class ProbeBus
{
public:
	explicit ProbeBus(int bus) : _device(px4_i2cbus_initialize(bus)) {}
	~ProbeBus()
	{
		// STM32H7 NuttX uses per-bus refs. Releasing our handle preserves any
		// already-running pressure/other driver references and bus hardware.
		if (_device) { px4_i2cbus_uninitialize(_device); }
	}
	ProbeBus(const ProbeBus &) = delete;
	ProbeBus &operator=(const ProbeBus &) = delete;
	bool valid() const { return _device != nullptr; }

	int write_command(uint16_t command)
	{
		uint8_t data[2] {static_cast<uint8_t>(command >> 8), static_cast<uint8_t>(command)};
		return transfer(data, sizeof(data), false);
	}

	int read_response(uint8_t *data, size_t length) { return transfer(data, length, true); }

	int wait_us(uint32_t duration)
	{
		// This runs in the calling console task, never a sensor work queue.
		// Signals may interrupt usleep: enforce the minimum delay, while
		// bounding interruptions to four attempts instead of busy spinning.
		const hrt_abstime start = hrt_absolute_time();

		if (start > UINT64_MAX - duration) { return -ERANGE; }

		const hrt_abstime deadline = start + duration;

		for (unsigned attempt = 0; attempt < 4; ++attempt) {
			const hrt_abstime now = hrt_absolute_time();

			if (now >= deadline) { return 0; }

			const int rc = px4_usleep(static_cast<useconds_t>(deadline - now));

			if (rc != 0 && errno != EINTR) { return errno ? -errno : -EIO; }
		}

		return hrt_absolute_time() >= deadline ? 0 : -EINTR;
	}

private:
	int transfer(uint8_t *data, size_t length, bool read)
	{
		i2c_msg_s msg{};
		msg.frequency = FREQUENCY_HZ;
		msg.addr = shtc3::I2C_ADDRESS;
		msg.flags = read ? I2C_M_READ : 0;
		msg.buffer = data;
		msg.length = static_cast<int>(length);
		// NuttX serializes each transfer with the existing per-bus semaphore.
		// Direct API preserves errno and a per-message 100 kHz clock, without
		// device::I2C's hidden probe or global bus-clock setting. No retry or
		// I2C_RESET call is made here, including on NACK/timeout.
		return I2C_TRANSFER(_device, &msg, 1);
	}

	i2c_master_s *_device;
};

int usage()
{
	PRINT_MODULE_DESCRIPTION(
		"One-shot SHTC3 ID check at 0x70, 100 kHz. Maintenance only. "
		"No other driver may access 0x70 during this command. "
		"No scan, reset, measurements, parameters or topic publication.");
	PRINT_MODULE_USAGE_NAME_SIMPLE("shtc3_probe", "command");
	PRINT_MODULE_USAGE_PARAM_INT('b', 2, 1, 4, "Physical I2C bus (required)", false);
	return PX4_ERROR;
}
} // namespace

extern "C" __EXPORT int shtc3_probe_main(int argc, char *argv[]);
int shtc3_probe_main(int argc, char *argv[])
{
	// Strict, explicit bus selection avoids touching a default or every bus.
	if (argc != 3 || std::strcmp(argv[1], "-b") != 0) { return usage(); }

	char *end = nullptr;
	errno = 0;
	const long bus_number = std::strtol(argv[2], &end, 10);

	if (errno || !end || end == argv[2] || *end || bus_number < 1 || bus_number > 4) { return usage(); }

	bool expected = false;

	if (!probe_running.compare_exchange(&expected, true)) {
		PX4_ERR("diagnostic already running; no bus access");
		return PX4_ERROR;
	}

	ProbeGuard guard;
	ProbeBus bus(static_cast<int>(bus_number));

	if (!bus.valid()) {
		PX4_ERR("result=UNCONFIRMED stage=BUS_INIT bus=%ld no transfer attempted", bus_number);
		return PX4_ERROR;
	}

	PX4_INFO("bus=%ld address=0x70 frequency_hz=100000; one-shot, no retries or I2C_RESET calls", bus_number);
	const hrt_abstime start = hrt_absolute_time();
	const shtc3_probe::Result result = shtc3_probe::run(bus);
	const hrt_abstime elapsed = hrt_elapsed_time(&start);
	char report[224] {};
	shtc3_probe::format_result(result, report, sizeof(report));
	PX4_INFO("%s", report);
	PX4_INFO("elapsed_us=%" PRIu64 " (includes waits/OS scheduling; not a WCET)", elapsed);

	if (result.sleep_attempted && result.sleep_rc != 0) {
		PX4_WARN("SHTC3 ID confirmed, but SLEEP failed; sensor state unknown");
	}

	if (!result.matched) {
		PX4_WARN("connection not confirmed; this does not prove a wiring fault");
	}

	return result.matched ? PX4_OK : PX4_ERROR;
}
