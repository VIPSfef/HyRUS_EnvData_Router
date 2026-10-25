# SENS_ATMOS production stream boundary checks

This native harness compiles and calls the actual `../SENS_ATMOS.hpp`.
Only uORB storage, the boot clock, MAVLink channel/version and the send API are
stubbed. The stream decision logic is not copied into the test. It checks:

- MAVLink 1 neither sends ID 8009 nor consumes the pending uORB update;
- MAVLink 2 sends fresh Celsius, percent RH and the original boot-time sample
  timestamp without conversion to kelvin/fraction or publication time;
- only sensor_hygrometer instance zero is selected;
- unadvertised or already-consumed observations do not send;
- zero/future timestamps, age over one second, NaN/infinite temperatures or
  humidities, temperature below absolute zero and RH outside 0..100 reject;
- valid age/range boundaries and negative Celsius are retained.

```sh
cmake -S src/px4/sens_atmos/tests -B build/sens-atmos-stream-tests -G Ninja
cmake --build build/sens-atmos-stream-tests
ctest --test-dir build/sens-atmos-stream-tests --output-on-failure
```

Windows Clang 22 and Linux GCC 13.3 each passed 79 contract assertions.
This test does not test MAVLink byte serialization, a physical sensor, I2C
clock speed or real-time scheduling. The real PX4 ARM build uses its generated
MAVLink definitions; live TCP verification must separately compare received
raw observations and independently computed statistics. No sensor data or
physical device handles are used by this test.
