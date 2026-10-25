// Test-only boot clock: no PX4 process or physical clock is queried.
#pragma once
#include <cstdint>
namespace fake { extern uint64_t now_us; }
inline uint64_t hrt_absolute_time() { return fake::now_us; }
