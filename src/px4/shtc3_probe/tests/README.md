# SHTC3 one-shot probe protocol checks

These tests run the same `SHTC3Probe.hpp` function and result formatter used by
`shtc3_probe`; they never access COM ports, I2C hardware, or a running PX4.
The production `SHTC3Protocol.hpp` supplies commands, CRC and the product mask.
No existing sensor-driver source is copied or changed.

```sh
cmake -S src/px4/shtc3_probe/tests -B build/shtc3-probe-tests -G Ninja
cmake --build build/shtc3-probe-tests
ctest --test-dir build/shtc3-probe-tests --output-on-failure
```

The fake bus verifies the exact command order, two waits of at least 240 us,
three-byte ID read, a maximum of four transfers, and no retry/reset/measurement
command. All 65,536 product IDs are checked against the documented bit mask;
all 24 single-bit corruptions of a valid ID response must fail CRC. Error cases
cover each IO stage, both waits and SLEEP. Only a CRC-and-ID match is confirmed;
SLEEP failure preserves that verdict and reports cleanup failure separately.
No SLEEP command is sent to a device whose identity was not validated.

The tests also check the real console result strings and bounded formatting.
A Windows Clang 22 build and a Linux GCC 13.3 build each passed 1,444,257
assertions. These are host checks, not measurements of hardware reliability,
voltage compatibility, scheduler timing or worst-case execution time.

The command uses fixed memory in its protocol and result (20-byte result on
both hosts), with no per-step allocation. Its NuttX bus-handle API allocates
one small instance and releases that instance on exit. It performs at most
117 bus clocks (1.17 ms at 100 kHz) plus two 300 us waits in a successful run,
before bus contention, platform timeouts, task scheduling and console output.
The OS bus semaphore is released between transfers and during the waits.
