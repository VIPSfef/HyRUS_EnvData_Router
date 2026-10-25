// Test-only message storage: production header includes this via stubs/.
#pragma once
#include <cstdint>
struct sensor_hygrometer_s {
	uint64_t timestamp{0};
	uint64_t timestamp_sample{0};
	uint32_t device_id{0};
	float temperature{0.f};
	float humidity{0.f};
};
