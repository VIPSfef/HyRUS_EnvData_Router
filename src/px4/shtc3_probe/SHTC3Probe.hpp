// SPDX-License-Identifier: BSD-3-Clause
// One-shot diagnostic, sharing the production driver's command/CRC/ID rules.
// This is not another sensor state machine and never measures or resets.
#pragma once

#include <SHTC3Protocol.hpp>
#include <cstdio>

namespace shtc3_probe
{
enum class Stage : uint8_t { Wake, WakeWait, ReadIdCommand, ReadIdWait, ReadIdResponse, Crc, ProductId, Complete };

struct Result {
	Stage stage{Stage::Wake};
	int operation_rc{0};
	int sleep_rc{0};
	uint16_t id{0};
	uint8_t crc_received{0};
	uint8_t crc_expected{0};
	bool response_received{false};
	bool matched{false};
	bool sleep_attempted{false};
};

inline const char *stage_name(Stage stage)
{
	switch (stage) {
	case Stage::Wake: return "WAKE";

	case Stage::WakeWait: return "WAKE_WAIT";

	case Stage::ReadIdCommand: return "READ_ID_COMMAND";

	case Stage::ReadIdWait: return "READ_ID_WAIT";

	case Stage::ReadIdResponse: return "READ_ID_RESPONSE";

	case Stage::Crc: return "CRC";

	case Stage::ProductId: return "PRODUCT_ID";

	case Stage::Complete: return "COMPLETE";
	}

	return "INVALID";
}

// Bus: int write_command(uint16_t), read_response(uint8_t*,size_t), wait_us(uint32_t).
// Success is exactly zero. Each command/read is one transfer, with no retries.
// At most four transfers. Cleanup only targets an ID-and-CRC validated SHTC3:
// a responding but different device at 0x70 must not receive a SLEEP command.
template<typename Bus>
Result run(Bus &bus)
{
	Result result;
	result.operation_rc = bus.write_command(shtc3::WAKE);

	if (result.operation_rc != 0) { return result; }

	result.stage = Stage::WakeWait;
	result.operation_rc = bus.wait_us(shtc3::WAKE_US);

	if (result.operation_rc != 0) { return result; }

	result.stage = Stage::ReadIdCommand;
	result.operation_rc = bus.write_command(shtc3::READ_ID);

	if (result.operation_rc != 0) { return result; }

	// Separate write and read, with a conservative 300 us guard; do not depend
	// on repeated-START register-read behaviour used by generic scanners.
	result.stage = Stage::ReadIdWait;
	result.operation_rc = bus.wait_us(shtc3::WAKE_US);

	if (result.operation_rc != 0) { return result; }

	uint8_t data[3] {};
	result.stage = Stage::ReadIdResponse;
	result.operation_rc = bus.read_response(data, sizeof(data));

	if (result.operation_rc != 0) { return result; }

	result.response_received = true;
	result.id = static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
	result.crc_received = data[2];
	result.crc_expected = shtc3::Core::crc8(data, 2);
	result.stage = Stage::Crc;

	if (result.crc_received != result.crc_expected) { return result; }

	result.stage = Stage::ProductId;

	if (!shtc3::Core::valid_id(data)) { return result; }

	result.matched = true;
	result.stage = Stage::Complete;
	result.sleep_attempted = true;
	result.sleep_rc = bus.write_command(shtc3::SLEEP);
	return result;
}

// Shared by the actual console command and native tests: a WAKE ACK alone is
// never reported as a successful sensor identification. IDs are product IDs,
// not serial numbers. Sleep failure is independent of the presence verdict.
inline int format_result(const Result &result, char *buffer, size_t capacity)
{
	if (result.response_received) {
		return std::snprintf(buffer, capacity,
				     "result=%s stage=%s id=0x%04X crc_rx=0x%02X crc_expected=0x%02X operation_rc=%d sleep=%s sleep_rc=%d",
				     result.matched ? "MATCH" : "UNCONFIRMED", stage_name(result.stage),
				     static_cast<unsigned>(result.id), static_cast<unsigned>(result.crc_received),
				     static_cast<unsigned>(result.crc_expected), result.operation_rc,
				     result.sleep_attempted ? (result.sleep_rc == 0 ? "OK" : "FAILED") : "NOT_ATTEMPTED",
				     result.sleep_rc);
	}

	return std::snprintf(buffer, capacity,
			     "result=UNCONFIRMED stage=%s id=unavailable crc_rx=unavailable crc_expected=unavailable operation_rc=%d sleep=NOT_ATTEMPTED",
			     stage_name(result.stage), result.operation_rc);
}
} // namespace shtc3_probe
