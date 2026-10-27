#ifndef MI_BATTERY_H
#define MI_BATTERY_H

#include <stdint.h>

/* Receipt freshness, independent of the vehicle/process elapsed time. */
#define MI_BATTERY_FRESH_MS UINT64_C(5000)
enum {
    MI_BATTERY_HAVE_VOLTAGE = 1u << 0,
    MI_BATTERY_HAVE_REMAINING = 1u << 1
};
typedef enum {
    MI_BATTERY_SOURCE_NONE,
    MI_BATTERY_SOURCE_SYS_STATUS,
    MI_BATTERY_SOURCE_BATTERY_STATUS
} mi_battery_source;

typedef struct {
    uint32_t voltage_mv; /* Whole pack, summed cells; not limited to uint16. */
    uint8_t remaining_percent, flags, source, id;
    uint64_t received_ms, voltage_received_ms, remaining_received_ms;
} mi_battery;

/* Flags describe the latest coherent sample, never inferred percentages.
 * A flag is cleared when a new sample explicitly reports unknown/invalid.
 * Timers only advance for their own valid field; heartbeat cannot refresh them. */
static inline int mi_battery_field_fresh(const mi_battery *b, unsigned field, uint64_t now)
{
    uint64_t received;
    if (!b || !(b->flags & field)) return 0;
    if (field == MI_BATTERY_HAVE_VOLTAGE) received = b->voltage_received_ms;
    else if (field == MI_BATTERY_HAVE_REMAINING) received = b->remaining_received_ms;
    else return 0;
    return now >= received && now - received <= MI_BATTERY_FRESH_MS;
}

#endif
