#ifndef TIME_SYNC_H
#define TIME_SYNC_H

#include <stdint.h>

#define TC_ANCHORS 16u
#ifndef TC_EXTRAPOLATE_US
#define TC_EXTRAPOLATE_US UINT64_C(10000000)
#endif
#ifndef TC_DRIFT_LIMIT_PPM
#define TC_DRIFT_LIMIT_PPM 500.0
#endif

/* UTC is Unix microseconds supplied by the decoder, not inferred from its
 * magnitude. A tc_clock belongs to one Pixhawk boot/session. Convert GPS time
 * to UTC upstream and include GNSS, boot-ms quantization and source timestamp
 * errors in gnss_uncert_us. RTT is the measured clock-exchange round trip.
 * All quality/error values below are ESTIMATES, not coverage guarantees.
 * Caller owns synchronization. This module allocates no memory. */
typedef struct {
    uint64_t boot_us, utc_us;
    uint32_t gnss_uncert_us, rtt_us;
} tc_point;

typedef struct {
    uint32_t count;
    uint64_t oldest_boot_us, last_boot_us, last_utc_us;
    /* Rounded fitted UTC-minus-boot offset evaluated at last_boot_us. */
    int64_t offset_us;
    double drift_ppm;
    /* sqrt(SSE/(count-2)); zero for fewer than three anchors is not proof
     * of zero clock error. max_abs_residual_us includes all stored anchors. */
    double residual_std_us, max_abs_residual_us, roundtrip_mean_us;
    uint32_t roundtrip_max_us, anchor_uncert_max_us;
} tc_report;

typedef struct {
    tc_point anchor[TC_ANCHORS];
    tc_report model;
    /* Internal centered model: UTC = base_utc + (boot-base_boot) +
     * intercept_us + slope * (boot-base_boot). */
    uint64_t base_boot_us, base_utc_us;
    double intercept_us, slope;
} tc_clock;

void tc_init(tc_clock *clock);
/* Returns 1 on acceptance; 0 leaves the entire clock unchanged. Both boot
 * and UTC anchors must increase strictly; UTC zero means unavailable.
 * Fits the newest 16 anchors using centered ordinary least squares. Rejects
 * clock jumps inconsistent with the current estimated errors/drift allowance
 * and fitted drift outside TC_DRIFT_LIMIT_PPM. Fresh anchors can recover a
 * stale model. Reboots, UTC steps/leap discontinuities require explicit reset. */
int tc_anchor(tc_clock *clock, uint64_t boot_us, uint64_t utc_us,
              uint32_t gnss_uncert_us, uint32_t rtt_us);
/* Allows interpolation from oldest through last anchor, and extrapolation
 * through last+TC_EXTRAPOLATE_US. Neither output changes on failure.
 * Error indicator = max input error + ceil(max RTT/2) + max fit residual +
 * distance to nearest stored anchor * drift limit + numerical roundoff guard.
 * This is a conservative engineering ESTIMATE under the supplied assumptions,
 * not a verified statistical interval or hardware-specified absolute bound. */
int tc_estimate(const tc_clock *clock, uint64_t boot_us, uint64_t *utc_us,
                uint32_t *estimated_uncert_us);
/* Returns 0 without changing report until an anchor has been accepted. */
int tc_get_report(const tc_clock *clock, tc_report *report);

#endif
