#ifndef SERIAL_BAUD_H
#define SERIAL_BAUD_H

#include <stddef.h>

/* One supported-rate list for configuration, discovery and native mapping.
 * Probe every port at the preferred rate before expanding in this order. */
#define SERIAL_BAUD_RATES(X) \
    X(115200) X(57600) X(230400) X(460800) \
    X(921600) X(38400) X(19200) X(9600)

enum {
#define SERIAL_BAUD_ENUM(rate) SERIAL_BAUD_##rate = rate,
    SERIAL_BAUD_RATES(SERIAL_BAUD_ENUM)
#undef SERIAL_BAUD_ENUM
    SERIAL_BAUD_DEFAULT = SERIAL_BAUD_115200
};

static const unsigned serial_baud_rates[] = {
#define SERIAL_BAUD_VALUE(rate) SERIAL_BAUD_##rate,
    SERIAL_BAUD_RATES(SERIAL_BAUD_VALUE)
#undef SERIAL_BAUD_VALUE
};
enum { SERIAL_BAUD_COUNT = sizeof(serial_baud_rates) / sizeof(serial_baud_rates[0]) };

static inline int serial_baud_valid(unsigned baud)
{
    size_t i;
    for (i = 0; i < SERIAL_BAUD_COUNT; ++i)
        if (serial_baud_rates[i] == baud) return 1;
    return 0;
}

/* Zero marks exhaustion; the preferred rate occurs exactly once. */
static inline unsigned serial_baud_probe(unsigned preferred, unsigned index)
{
    size_t i;
    if (index >= SERIAL_BAUD_COUNT || !serial_baud_valid(preferred)) return 0;
    if (!index) return preferred;
    for (i = 0; i < SERIAL_BAUD_COUNT; ++i)
        if (serial_baud_rates[i] != preferred && !--index) return serial_baud_rates[i];
    return 0;
}

#endif
