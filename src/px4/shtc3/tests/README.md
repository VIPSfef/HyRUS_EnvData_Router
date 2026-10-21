# SHTC3 tests without hardware

These tests execute the **same portable protocol/state core used by the PX4
SHTC3 driver**, with a fake I2C bus and an explicit microsecond clock. They do
not demonstrate electrical compatibility, sensor accuracy, flight hardware
operation, real bus timing, or environmental response time.

The fixtures follow [Sensirion SHTC3 datasheet, version 4, December 2022](https://sensirion.com/media/documents/643F9C8E/63A5A436/Datasheet_SHTC3.pdf),
sections 3 and 5. Tests check the official CRC examples and the six-byte example
from Figure 7, reordered into the temperature-first format selected by the
driver. The CRC reference uses polynomial long division, independently of the
production CRC loop. Conversion references use a denominator of 65536.

The fake bus supplies replies and records transfers. It does not contain a
second driver state machine. Assertions observe command order, transfer sizes,
publish decisions, deadlines, fault counters, and bounded retry behavior in the
production core. Observation data stays in test-process memory.

Compile the native test as a standalone C++ program after the production header
has been copied beside this directory. Build outputs belong under `build/work`,
not beside source files. The ordinary router application build does not include
this PX4-specific test or require a C++ compiler.

## Standalone build

From the repository root on Windows (a native C++17 compiler is required):

```powershell
cmake -S src/px4/shtc3/tests -B build/shtc3-tests-windows
cmake --build build/shtc3-tests-windows --config Release
ctest --test-dir build/shtc3-tests-windows -C Release --output-on-failure
```

On Linux:

```sh
cmake -S src/px4/shtc3/tests -B build/shtc3-tests-linux -DCMAKE_BUILD_TYPE=Release
cmake --build build/shtc3-tests-linux
ctest --test-dir build/shtc3-tests-linux --output-on-failure
```

The core performs at most one I2C transfer per scheduled tick. A successful
steady-state cycle uses three two-byte commands and one six-byte read, about
144 I2C clock pulses before START/STOP and software overhead: 0.36 ms at 400 kHz
or 1.44 ms at 100 kHz. Conversion waits are scheduled for 13 ms; wake/reset waits
are scheduled for 300 us. They do not busy-wait or sleep inside the work item.
The nominal 100 ms cycle starts at a successful wake command, so the 300 us
wake wait fits inside the period. The initial ID read adds a one-time startup
phase difference. The fake-clock test checks an exact 100 ms steady-state
measurement interval; real scheduling and bus overhead remain hardware-dependent.
This is not a real-time hardware latency guarantee.
