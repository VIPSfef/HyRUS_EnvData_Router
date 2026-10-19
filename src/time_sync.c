#include "time_sync.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

static int difference(uint64_t a, uint64_t b, int64_t *result)
{
    uint64_t magnitude = a >= b ? a - b : b - a;
    if (a >= b) {
        if (magnitude > (uint64_t)INT64_MAX) return 0;
        *result = (int64_t)magnitude;
    } else {
        if (magnitude > (uint64_t)INT64_MAX + 1u) return 0;
        *result = magnitude == (uint64_t)INT64_MAX + 1u ?
                  INT64_MIN : -(int64_t)magnitude;
    }
    return 1;
}

static double roundoff(const tc_clock *c, uint64_t boot_us)
{
    double span = (double)(boot_us - c->base_boot_us);
    return 1.0 + ceil(8.0 * DBL_EPSILON *
                      (span + fabs(c->intercept_us) + fabs(c->slope * span) + 1.0));
}

/* Keep the large epoch in integer arithmetic; only the small fitted
 * correction is rounded. Cancellation precedes overflow checks. */
static int predict(const tc_clock *c, uint64_t boot_us, uint64_t *utc_us)
{
    uint64_t elapsed, amount, result;
    double correction;
    int64_t rounded;
    if (!c || !c->model.count || boot_us < c->base_boot_us) return 0;
    elapsed = boot_us - c->base_boot_us;
    correction = round(c->intercept_us + c->slope * (double)elapsed);
    if (!isfinite(correction) || correction < -9223372036854775808.0 ||
        correction >= 9223372036854775808.0) return 0;
    rounded = (int64_t)correction;
    if (rounded >= 0) {
        amount = (uint64_t)rounded;
        if (elapsed > UINT64_MAX - c->base_utc_us) return 0;
        result = c->base_utc_us + elapsed;
        if (amount > UINT64_MAX - result) return 0;
        result += amount;
    } else {
        amount = (uint64_t)(-(rounded + 1)) + 1u;
        if (elapsed >= amount) {
            amount = elapsed - amount;
            if (amount > UINT64_MAX - c->base_utc_us) return 0;
            result = c->base_utc_us + amount;
        } else {
            amount -= elapsed;
            if (amount > c->base_utc_us) return 0;
            result = c->base_utc_us - amount;
        }
    }
    *utc_us = result;
    return 1;
}

void tc_init(tc_clock *c)
{
    if (c) memset(c, 0, sizeof(*c));
}

int tc_estimate(const tc_clock *c, uint64_t boot_us, uint64_t *utc_us,
                uint32_t *estimated_uncert_us)
{
    uint64_t result, distance = UINT64_MAX;
    double error;
    uint32_t i;
    if (!c || !utc_us || !estimated_uncert_us || !c->model.count ||
        boot_us < c->model.oldest_boot_us ||
        (boot_us > c->model.last_boot_us &&
         boot_us - c->model.last_boot_us > TC_EXTRAPOLATE_US)) return 0;
    for (i = 0; i < c->model.count; ++i) {
        uint64_t t = c->anchor[i].boot_us;
        uint64_t d = boot_us >= t ? boot_us - t : t - boot_us;
        if (d < distance) distance = d;
    }
    error = (double)c->model.anchor_uncert_max_us +
            ceil((double)c->model.roundtrip_max_us / 2.0) +
            c->model.max_abs_residual_us +
            (double)distance * TC_DRIFT_LIMIT_PPM / 1e6 + roundoff(c, boot_us);
    if (!isfinite(error) || error < 0 || ceil(error) > (double)UINT32_MAX ||
        !predict(c, boot_us, &result)) return 0;
    *utc_us = result;
    *estimated_uncert_us = (uint32_t)ceil(error);
    return 1;
}

static int fit(tc_clock *c)
{
    double x[TC_ANCHORS], y[TC_ANCHORS];
    double mean_x = 0, mean_y = 0, sxx = 0, sxy = 0, sse = 0;
    uint64_t rtt_sum = 0, last_estimate;
    uint32_t i, ignored_error, n = c->model.count;
    tc_report r;
    memset(&r, 0, sizeof(r));
    r.count = n;
    c->base_boot_us = r.oldest_boot_us = c->anchor[0].boot_us;
    c->base_utc_us = c->anchor[0].utc_us;
    r.last_boot_us = c->anchor[n - 1].boot_us;
    r.last_utc_us = c->anchor[n - 1].utc_us;
    for (i = 0; i < n; ++i) {
        int64_t offset_delta;
        uint64_t elapsed = c->anchor[i].boot_us - c->base_boot_us;
        uint64_t utc_elapsed = c->anchor[i].utc_us - c->base_utc_us;
        if (!difference(utc_elapsed, elapsed, &offset_delta)) return 0;
        x[i] = (double)elapsed;
        y[i] = (double)offset_delta;
        mean_x += x[i]; mean_y += y[i];
        rtt_sum += c->anchor[i].rtt_us;
        if (c->anchor[i].rtt_us > r.roundtrip_max_us)
            r.roundtrip_max_us = c->anchor[i].rtt_us;
        if (c->anchor[i].gnss_uncert_us > r.anchor_uncert_max_us)
            r.anchor_uncert_max_us = c->anchor[i].gnss_uncert_us;
    }
    mean_x /= n; mean_y /= n;
    for (i = 0; i < n; ++i) {
        double dx = x[i] - mean_x;
        sxx += dx * dx;
        sxy += dx * (y[i] - mean_y);
    }
    c->slope = n > 1 ? sxy / sxx : 0;
    c->intercept_us = mean_y - c->slope * mean_x;
    r.drift_ppm = c->slope * 1e6;
    if (!isfinite(c->slope) || !isfinite(c->intercept_us) ||
        fabs(r.drift_ppm) > TC_DRIFT_LIMIT_PPM + 1e-6) return 0;
    for (i = 0; i < n; ++i) {
        double residual = y[i] - (c->intercept_us + c->slope * x[i]);
        sse += residual * residual;
        if (fabs(residual) > r.max_abs_residual_us)
            r.max_abs_residual_us = fabs(residual);
    }
    r.residual_std_us = n > 2 ? sqrt(sse / (n - 2)) : 0;
    r.roundtrip_mean_us = (double)rtt_sum / n;
    c->model = r;
    if (!tc_estimate(c, r.last_boot_us, &last_estimate, &ignored_error) ||
        !difference(last_estimate, r.last_boot_us, &c->model.offset_us)) return 0;
    return isfinite(r.residual_std_us);
}

int tc_anchor(tc_clock *c, uint64_t boot_us, uint64_t utc_us,
              uint32_t gnss_uncert_us, uint32_t rtt_us)
{
    tc_clock next;
    uint32_t index;
    if (!c || !utc_us) return 0;
    if (c->model.count) {
        uint64_t predicted;
        double residual, allowance;
        if (boot_us <= c->model.last_boot_us || utc_us <= c->model.last_utc_us ||
            !predict(c, boot_us, &predicted)) return 0;
        residual = (double)(utc_us >= predicted ? utc_us - predicted : predicted - utc_us);
        allowance = (double)c->model.anchor_uncert_max_us + gnss_uncert_us +
                    ceil((double)c->model.roundtrip_max_us / 2.0) +
                    ceil((double)rtt_us / 2.0) + c->model.max_abs_residual_us +
                    (double)(boot_us - c->model.last_boot_us) *
                    TC_DRIFT_LIMIT_PPM / 1e6 + roundoff(c, boot_us);
        if (residual > allowance) return 0;
    }
    next = *c;
    if (next.model.count == TC_ANCHORS) {
        memmove(next.anchor, next.anchor + 1, (TC_ANCHORS - 1u) * sizeof(next.anchor[0]));
        --next.model.count;
    }
    index = next.model.count++;
    next.anchor[index].boot_us = boot_us;
    next.anchor[index].utc_us = utc_us;
    next.anchor[index].gnss_uncert_us = gnss_uncert_us;
    next.anchor[index].rtt_us = rtt_us;
    if (!fit(&next)) return 0;
    *c = next;
    return 1;
}

int tc_get_report(const tc_clock *c, tc_report *report)
{
    if (!c || !report || !c->model.count) return 0;
    *report = c->model;
    return 1;
}
