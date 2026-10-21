#ifndef WEATHER_H
#define WEATHER_H

#include <stddef.h>
#include <stdint.h>

#define WEATHER_CAPACITY 128u
#define WEATHER_GROUP_CAPACITY 16u
#define WEATHER_SI_REGISTRY 1u
#define WEATHER_QUALITY_TIME_UNKNOWN UINT32_C(0x08000000)
#define WEATHER_QUALITY_POSITION_UNKNOWN UINT32_C(0x04000000)

enum { WEATHER_TEMPERATURE = 1, WEATHER_HUMIDITY = 2 };

/* SI registry 1: temperature is kelvin, relative humidity is a fraction [0,1].
 * t_us is the sensor acquisition time on the Pixhawk boot clock, NOT reception
 * time. An unknown acquisition time MUST set has_time=0 and has_position=0.
 * p and v are local ENU meters and meters/second in a session-specific origin.
 * NaN measurement_variance/pos_error_m and UINT32_MAX time_uncert_us mean
 * unknown uncertainty. Variance describes a single measurement, not variation
 * between samples. sensor_id/instance identify a sensor within the enclosing
 * source system/component/session when the wire supplies them; zero can mean
 * unknown metadata and does not identify a chip. quality bits above are reserved.
 * message_id identifies the actual wire protocol (8009 SENS_ATMOS, 12920
 * HYGROMETER_SENSOR); 0 is unknown metadata for API callers. It is not a
 * hardware identifier. Different wire protocols always form separate groups.
 * seq breaks ties; choose increasing, unique sequence numbers at the source. */
typedef struct {
    uint64_t t_us;
    uint32_t sensor_id, seq, quality, time_uncert_us;
    double value, measurement_variance, p[3];
    float v[3], pos_error_m;
    uint8_t metric, sensor_instance, has_time, has_position;
    uint32_t message_id;
} weather_sample;

typedef struct {
    uint32_t sensor_id, message_id, n, raw_n, raw_dropped, time_known_n;
    uint64_t t_start_us, t_end_us;
    double mean, m2, min, max;
    weather_sample representative;
    uint8_t metric, sensor_instance;
} weather_group;

typedef struct {
    uint64_t host_window_ms;
    uint32_t n, raw_n, raw_dropped, group_n;
    weather_group group[WEATHER_GROUP_CAPACITY];
    weather_sample raw[WEATHER_CAPACITY];
    uint8_t prepared;
} weather_batch;

/* Caller-owned fixed storage; no heap or global state. A batch must belong to
 * exactly one source system/component/clock session and navigation origin.
 * host_window_ms labels the collector's 1 s scheduling window, independently
 * of acquisition timestamps. Its enclosing envelope defines the host clock. */
void weather_batch_init(weather_batch *batch, uint64_t host_window_ms);
int weather_sample_valid(const weather_sample *sample);
/* O(WEATHER_GROUP_CAPACITY), one fixed sample copy. Invalid samples, group
 * exhaustion, or uint32 counters at their limit reject without mutation.
 * All accepted samples update Welford statistics, even when raw storage is
 * full. The retained raw subset is the first WEATHER_CAPACITY arrivals. */
int weather_batch_add(weather_batch *batch, const weather_sample *sample);
/* Worker-side in-place heapsort, O(raw_n log raw_n + group_n log group_n).
 * Sort order is metric, wire message ID, sensor ID, instance, known time first, t_us, seq, value.
 * Call after final add, before serializing. An add invalidates preparation. */
void weather_batch_prepare(weather_batch *batch);
/* Whole JSON object with si_registry, host_window_ms (decimal string), counts,
 * measurements:{temperature:[sensor groups],humidity:[sensor groups]}.
 * Each group contains an actual representative, independent statistics, and
 * retained sample objects. No mean is attached to an actual sample position.
 * variance is unbiased SAMPLE variance (n-1), null for n<2; acquisition window
 * bounds concern only time_known_n samples, null if none. Unknown acquisition
 * times, positions and uncertainties remain null. Units follow SI registry 1
 * and are not repeated. Combined positioning uncertainty is a speed-based
 * estimate, not a guarantee. Representative ranking: known time+position,
 * known time, known combined positioning error then its magnitude, known
 * measurement variance then magnitude, known timestamp error then magnitude,
 * deterministic sample order. JSON uses the C numeric locale.
 * Returns bytes without trailing NUL, or -1. Failure clears buf[0]; NEVER send
 * partial output. Runtime is O(group_n * raw_n) with both capacities fixed. */
int weather_batch_json(const weather_batch *batch, char *buf, size_t cap);

#endif
