#include "mavlink_input.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct { uint32_t id; uint8_t crc, base_len, max_len; } mi_message_info;
#include "vendor/mavlink_common_crc.h"

#define MI_PI 3.14159265358979323846264338327950288
#define MI_BOOT_RESET_US UINT64_C(5000000)
#define MI_UNIX_MIN_US UINT64_C(946684800000000)
#define MI_UNIX_MAX_US UINT64_C(4102444800000000)

static uint16_t u16(const uint8_t *p)
{ return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t u32(const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t u64(const uint8_t *p)
{ return (uint64_t)u32(p) | ((uint64_t)u32(p + 4) << 32); }
static int16_t i16(const uint8_t *p)
{ uint16_t u = u16(p); return u <= INT16_MAX ? (int16_t)u : (int16_t)(-1 - (int32_t)(UINT16_MAX - u)); }
static int32_t i32(const uint8_t *p)
{ uint32_t u = u32(p); return u <= INT32_MAX ? (int32_t)u : -1 - (int32_t)(UINT32_MAX - u); }
static float f32(const uint8_t *p)
{ uint32_t u = u32(p); float f; memcpy(&f, &u, sizeof(f)); return f; }
static uint16_t crc_byte(uint16_t crc, uint8_t byte)
{
    uint8_t t = (uint8_t)(byte ^ (uint8_t)crc);
    t = (uint8_t)(t ^ (uint8_t)(t << 4));
    return (uint16_t)((crc >> 8) ^ ((uint16_t)t << 8) ^ ((uint16_t)t << 3) ^ (t >> 4));
}
static uint16_t checksum(const uint8_t *p, size_t n, uint8_t extra)
{
    uint16_t crc = UINT16_MAX;
    size_t j;
    for (j = 0; j < n; ++j) crc = crc_byte(crc, p[j]);
    return crc_byte(crc, extra);
}
static const mi_message_info *message_info(uint32_t id)
{
    size_t lo = 0, hi = sizeof(mi_common) / sizeof(mi_common[0]);
    while (lo < hi) {
        size_t m = lo + (hi - lo) / 2;
        if (mi_common[m].id == id) return mi_common + m;
        if (mi_common[m].id < id) lo = m + 1;
        else hi = m;
    }
    return NULL;
}
static void event(mi_input *m, mi_event_kind kind, uint64_t boot)
{
    mi_event e;
    if (!m->emit) return;
    memset(&e, 0, sizeof(e));
    e.kind = kind; e.sysid = m->report.selected_system;
    e.compid = m->report.selected_component; e.epoch = m->epoch;
    e.boot_us = boot;
    e.origin[0] = m->options.origin_lat; e.origin[1] = m->options.origin_lon;
    e.origin[2] = m->options.origin_alt;
    m->emit(m->context, &e);
}
void mi_options_default(mi_options *o)
{
    if (!o) return;
    memset(o, 0, sizeof(*o));
    o->system_time_uncert_us = UINT32_MAX;
    o->accel_bound_mps2 = 10.0;
}
void mi_init(mi_input *m, const mi_options *o, mi_emit_fn emit, void *context)
{
    if (!m) return;
    memset(m, 0, sizeof(*m));
    mi_options_default(&m->options);
    if (o) m->options = *o;
    if (!isfinite(m->options.accel_bound_mps2) || m->options.accel_bound_mps2 < 0 ||
        m->options.accel_bound_mps2 > 1e6) m->options.accel_bound_mps2 = 10.0;
    m->emit = emit; m->context = context; m->epoch = 1;
    m->gps_error_m = NAN;
    m->origin_set = !m->options.origin_auto;
    m->report.selected_system = m->options.target_system;
    m->report.selected_component = m->options.target_component;
}
void mi_link_reset(mi_input *m)
{ if (m) { m->used = 0; m->needed = 0; } }
void mi_get_report(const mi_input *m, mi_report *r)
{ if (m && r) { *r = m->report; r->pending = m->pending_count; } }

static mi_device *device(mi_input *m, uint8_t sysid, uint8_t compid, uint64_t now)
{
    unsigned j;
    mi_device *d;
    for (j = 0; j < m->report.devices; ++j)
        if (m->report.device[j].sysid == sysid && m->report.device[j].compid == compid) {
            m->report.device[j].last_seen_ms = now;
            return m->report.device + j;
        }
    if (m->report.devices == MI_DEVICE_CAP) { ++m->report.device_overflow; return NULL; }
    d = m->report.device + m->report.devices++;
    memset(d, 0, sizeof(*d));
    d->sysid = sysid; d->compid = compid; d->autopilot = 8; d->type = 255;
    d->last_seen_ms = now;
    return d;
}
static void reset_clock(mi_input *m)
{
    m->report.no_navigation += m->pending_count;
    m->pending_count = 0; m->nav_count = 0;
    m->high_boot_us = m->wrap_ms = m->last_emitted_us = m->last_watermark_us = 0;
    m->last_boot_ms = m->next_seq = m->emitted_seq = 0; m->have_boot = m->have_emitted = 0;
    m->gps_error_m = NAN; m->gps_accuracy_ms = 0;
    m->report.gps_fix = 0;
    m->report.have_navigation = 0;
    m->report.navigation_received_ms = m->report.navigation_boot_us = 0;
    m->report.watermark_us = m->report.boot_us = 0;
    ++m->epoch; ++m->report.reboots;
    event(m, MI_RESET, 0);
    /* Keep the local coordinate frame stable over a reboot on the same byte
     * source. Re-announce it so a new session has explicit origin metadata. */
    if (m->options.origin_auto && m->origin_set) event(m, MI_ORIGIN, 0);
}
/* MAVLink uint32 time_boot_ms wraps after 49.71 days. Small backwards steps
 * are reorder; a >5 s backwards jump is an explicit new source epoch. A stale
 * pre-reboot packet can trigger another reset, which is conservative: never
 * join observations across uncertain boot epochs. */
static uint64_t boot_ms(mi_input *m, uint32_t stamp)
{
    uint64_t value;
    if (m->have_boot && m->last_boot_ms > UINT32_C(0xf0000000) && stamp < UINT32_C(0x10000000))
        m->wrap_ms += UINT64_C(4294967296);
    value = (m->wrap_ms + stamp) * UINT64_C(1000);
    /* A late packet from just before wrap belongs to the preceding cycle. */
    if (m->have_boot && m->wrap_ms && m->last_boot_ms < UINT32_C(0x10000000) && stamp > UINT32_C(0xf0000000))
        value -= UINT64_C(4294967296000);
    if (m->have_boot && value + MI_BOOT_RESET_US < m->high_boot_us) {
        reset_clock(m); value = (uint64_t)stamp * UINT64_C(1000);
    }
    if (!m->have_boot || value >= m->high_boot_us) {
        m->last_boot_ms = stamp; m->high_boot_us = value;
        m->have_boot = 1; m->report.boot_us = value;
    }
    return value;
}
static int boot_usec(mi_input *m, uint64_t stamp, uint64_t *out)
{
    /* HIGHRES_IMU permits either epoch or boot time. An epoch timestamp
     * cannot be joined to boot navigation without a proven clock mapping. */
    if (stamp >= MI_UNIX_MIN_US) { ++m->report.no_clock; return 0; }
    if (m->have_boot && stamp + MI_BOOT_RESET_US < m->high_boot_us) reset_clock(m);
    if (!m->have_boot || stamp > m->high_boot_us) {
        m->high_boot_us = stamp; m->report.boot_us = stamp;
        m->wrap_ms = (stamp / UINT64_C(1000)) & ~UINT64_C(0xffffffff);
        m->last_boot_ms = (uint32_t)(stamp / UINT64_C(1000)); m->have_boot = 1;
    }
    *out = stamp;
    return 1;
}
static void add_nav(mi_input *m, const tb_nav *n)
{
    unsigned j, slot;
    if (!isfinite(n->p[0]) || !isfinite(n->p[1]) || !isfinite(n->p[2]) ||
        !isfinite(n->v[0]) || !isfinite(n->v[1]) || !isfinite(n->v[2]) ||
        fabs(n->p[0]) > 1e9 || fabs(n->p[1]) > 1e9 || fabs(n->p[2]) > 1e9) {
        ++m->report.nav_rejected; return;
    }
    for (j = 0; j < m->nav_count; ++j) {
        if (m->nav[j].t_us == n->t_us) { m->nav[j] = *n; return; }
        if (m->nav[j].t_us > n->t_us) break;
    }
    slot = j;
    if (m->nav_count == MI_NAV_CAP) {
        if (!slot) return;
        memmove(m->nav, m->nav + 1, (MI_NAV_CAP - 1u) * sizeof(m->nav[0]));
        --slot; --m->nav_count;
    }
    for (j = m->nav_count; j > slot; --j) m->nav[j] = m->nav[j - 1];
    m->nav[slot] = *n; ++m->nav_count;
}
static int name_ok(char *out, const uint8_t *name)
{
    unsigned j;
    for (j = 0; j < 10 && name[j]; ++j) {
        unsigned c = name[j];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
              (j && ((c >= '0' && c <= '9') || c == '.' || c == '-')))) return 0;
        out[j] = (char)c;
    }
    out[j] = '\0';
    return j != 0;
}
static void observation(mi_input *m, uint64_t stamp, uint64_t now,
                        const char *key, const char *unit, double value)
{
    mi_pending *p;
    unsigned j;
    if (!isfinite(value) || fabs(value) > 1e9) { ++m->report.invalid_value; return; }
    if ((m->last_watermark_us && stamp < m->last_watermark_us) ||
        (m->have_emitted && stamp < m->last_emitted_us)) {
        ++m->report.late_observation; return;
    }
    /* A duplicate key/time has no independent measurement identity. Keep
     * the first received value rather than silently replacing it. */
    for (j = 0; j < m->pending_count; ++j)
        if (m->pending[j].sample.t_us == stamp && !strcmp(m->pending[j].sample.key, key)) {
            ++m->report.duplicates; return;
        }
    if (m->pending_count == MI_PENDING_CAP) { ++m->report.pending_overflow; return; }
    p = m->pending + m->pending_count++;
    memset(p, 0, sizeof(*p));
    p->received_ms = now; p->sample.t_us = stamp;
    p->sample.seq = ++m->next_seq;
    p->sample.time_uncert_us = 1000; /* time_boot_ms quantization */
    p->sample.pos_error_m = NAN; p->sample.utc_uncert_us = UINT32_MAX;
    p->sample.quality = MI_QUALITY_CLOCK_UNSIGNED;
    (void)strcpy(p->sample.key, key); (void)strcpy(p->sample.unit, unit);
    p->sample.value = value;
}
static int align_sample(mi_input *m, tb_sample *s)
{
    unsigned j;
    for (j = 0; j < m->nav_count; ++j) {
        if (m->nav[j].t_us == s->t_us) {
            unsigned k;
            for (k = 0; k < 3; ++k) { s->p[k] = m->nav[j].p[k]; s->v[k] = m->nav[j].v[k]; }
            s->pos_error_m = m->nav[j].pos_error_m; s->nav_age_us = 0;
            if (!isfinite(s->pos_error_m)) s->quality |= MI_QUALITY_POSITION_UNKNOWN;
            return 1;
        }
        if (j && m->nav[j - 1].t_us < s->t_us && m->nav[j].t_us > s->t_us) {
            if (!tb_align(m->nav + j - 1, m->nav + j, s->t_us,
                          m->options.accel_bound_mps2, s)) return 0;
            if (!isfinite(s->pos_error_m)) s->quality |= MI_QUALITY_POSITION_UNKNOWN;
            return 1;
        }
    }
    return 0;
}
static void flush_pending(mi_input *m, uint64_t watermark, uint64_t now, int finishing)
{
    unsigned cache;
    /* Capture an actual bracketing solution as soon as it exists. Navigation
     * can be much faster than the 200ms reorder horizon; retaining coordinates
     * in the pending observation prevents a 32-entry nav ring from erasing
     * already available evidence before emission. No extrapolation is used. */
    for (cache = 0; cache < m->pending_count; ++cache)
        if (!m->pending[cache].aligned && align_sample(m, &m->pending[cache].sample))
            m->pending[cache].aligned = 1;
    /* At most 512 entries. Select-min preserves global event-time ordering,
     * independent of per-key or wire order. O(P^2 + P*N) per complete flush,
     * bounded to ~262k comparisons, no heap allocation or unbounded sort. */
    for (;;) {
        unsigned j, best = m->pending_count;
        for (j = 0; j < m->pending_count; ++j) {
            uint64_t t = m->pending[j].sample.t_us;
            if (best == m->pending_count || t < m->pending[best].sample.t_us ||
                (t == m->pending[best].sample.t_us && m->pending[j].sample.seq < m->pending[best].sample.seq))
                best = j;
        }
        if (best == m->pending_count) break;
        {
            tb_sample s = m->pending[best].sample;
            uint64_t age = now >= m->pending[best].received_ms ? now - m->pending[best].received_ms : 0;
            int source_safe = s.t_us <= watermark;
            int aligned;
            /* Receipt age only ends bounded reordering; it never creates a
             * navigation timestamp, coordinate, or a source boot watermark.
             * Drain a sorted prefix so no later observation overtakes it. */
            if (!finishing && !source_safe && age < 200) break;
            aligned = m->pending[best].aligned;
            if (!aligned && !finishing && !source_safe && age < 2000) break;
            m->pending[best] = m->pending[--m->pending_count];
            if (aligned) {
                mi_event e;
                memset(&e, 0, sizeof(e)); e.kind = MI_SAMPLE;
                e.sysid = m->report.selected_system; e.compid = m->report.selected_component;
                e.epoch = m->epoch; e.sample = s; e.boot_us = s.t_us;
                if (m->emitted_seq == UINT32_MAX) {
                    ++m->epoch; ++m->report.sequence_wraps; m->emitted_seq = 0;
                    event(m, MI_RESET, s.t_us); e.epoch = m->epoch;
                }
                e.sample.seq = ++m->emitted_seq;
                m->last_emitted_us = s.t_us; m->have_emitted = 1; ++m->report.observations;
                if (m->emit) m->emit(m->context, &e);
            } else ++m->report.no_navigation;
        }
    }
    /* No clock advance from host receipt time. A physically quiet source
     * remains at its last proven event-time watermark. Expire orphan samples
     * for visibility without inventing boot time or extrapolated positions. */
    {
        unsigned j = 0;
        while (j < m->pending_count) {
            if (now >= m->pending[j].received_ms && now - m->pending[j].received_ms > 2000) {
                m->pending[j] = m->pending[--m->pending_count]; ++m->report.no_navigation;
            } else ++j;
        }
    }
    if (watermark > m->last_watermark_us) {
        m->last_watermark_us = watermark; m->report.watermark_us = watermark;
        event(m, MI_WATERMARK, watermark);
    }
    m->report.pending = m->pending_count;
}
void mi_tick(mi_input *m, uint64_t now)
{
    uint64_t watermark;
    if (!m) return;
    watermark = m->high_boot_us > MI_REORDER_US ? m->high_boot_us - MI_REORDER_US : 0;
    flush_pending(m, watermark, now, 0);
}
void mi_finish(mi_input *m, uint64_t now)
{
    if (m) flush_pending(m, m->high_boot_us, now, 1);
}
static void global_navigation(mi_input *m, const uint8_t *p, uint64_t now)
{
    tb_nav nav;
    double lat = (double)i32(p + 4) * 1e-7, lon = (double)i32(p + 8) * 1e-7;
    double altitude = (double)i32(p + 12) * .001;
    double rad, sine, scale, meridian, prime, dlon;
    memset(&nav, 0, sizeof(nav));
    nav.t_us = boot_ms(m, u32(p));
    if (!isfinite(lat) || lat < -90 || lat > 90 || lon < -180 || lon > 180) {
        ++m->report.nav_rejected; return;
    }
    if (m->options.origin_auto && !m->origin_set) {
        m->options.origin_lat = lat; m->options.origin_lon = lon;
        m->options.origin_alt = altitude; m->origin_set = 1;
        event(m, MI_ORIGIN, nav.t_us);
    }
    if (!isfinite(m->options.origin_lat) || fabs(m->options.origin_lat) > 90 ||
        !isfinite(m->options.origin_lon) || fabs(m->options.origin_lon) > 180 ||
        !isfinite(m->options.origin_alt)) { ++m->report.nav_rejected; return; }
    rad = m->options.origin_lat * (MI_PI / 180.0); sine = sin(rad);
    scale = 1.0 - 0.0066943799901413165 * sine * sine;
    meridian = 6378137.0 * (1.0 - 0.0066943799901413165) / pow(scale, 1.5);
    prime = 6378137.0 / sqrt(scale);
    dlon = lon - m->options.origin_lon;
    if (dlon > 180) dlon -= 360;
    if (dlon < -180) dlon += 360;
    /* Local WGS84 radii projection; altitude is MSL, never relabeled as
     * ellipsoidal height. Regional approximation, not geoid conversion. */
    nav.p[0] = dlon * (MI_PI / 180.0) * prime * cos(rad);
    nav.p[1] = (lat - m->options.origin_lat) * (MI_PI / 180.0) * meridian;
    nav.p[2] = altitude - m->options.origin_alt;
    nav.v[0] = (float)i16(p + 22) * .01f; /* east */
    nav.v[1] = (float)i16(p + 20) * .01f; /* north */
    nav.v[2] = (float)i16(p + 24) * -.01f; /* up */
    nav.pos_error_m = NAN;
    if (isfinite(m->gps_error_m) && now >= m->gps_accuracy_ms && now - m->gps_accuracy_ms <= 2000) {
        double distance = hypot(nav.p[0], nav.p[1]);
        double projection_error = distance * distance / 6000000.0;
        nav.pos_error_m = (float)((double)m->gps_error_m + projection_error);
    }
    if(!m->report.have_navigation || nav.t_us>=m->report.navigation_boot_us) {
        m->report.lat = lat; m->report.lon = lon; m->report.altitude_msl_m = altitude;
        m->report.have_navigation = 1;
        m->report.navigation_received_ms = now;
        m->report.navigation_boot_us = nav.t_us;
    }
    add_nav(m, &nav);
}
static void packet(mi_input *m, uint32_t id, const uint8_t *p, uint8_t sysid,
                   uint8_t compid, uint8_t sequence, uint64_t now)
{
    mi_device *d = device(m, sysid, compid, now);
    uint64_t stamp;
    if (!sysid || !compid) return;
    if (d) {
        uint8_t delta = (uint8_t)(sequence - d->last_sequence);
        ++d->packets;
        if (d->have_sequence && !delta) { ++m->report.duplicates; return; }
        if (d->have_sequence && delta > 1 && delta < 128) {
            d->sequence_gaps += delta - 1u; m->report.sequence_gaps += delta - 1u;
        }
        if (!d->have_sequence || delta < 128) d->last_sequence = sequence;
        d->have_sequence = 1;
    }
    if (id == 0) {
        if (d) {
            d->custom_mode = u32(p); d->type = p[4]; d->autopilot = p[5];
            d->base_mode = p[6]; d->system_status = p[7];
            d->last_heartbeat_ms = now; d->have_heartbeat = 1;
        }
        if ((!m->report.selected_system || m->report.selected_system == sysid) &&
            (!m->report.selected_component || m->report.selected_component == compid) &&
            p[5] != 8 && p[4] != 6 && p[4] != 18) {
            m->report.selected_system = sysid; m->report.selected_component = compid;
        }
        return;
    }
    if (!m->report.selected_system || !m->report.selected_component ||
        m->report.selected_system != sysid) return;
    if (m->report.selected_component != compid) {
        if (id == 251 || id == 252 || id == 29 || id == 137 || id == 143 || id == 105)
            ++m->report.wrong_clock_component;
        return;
    }
    if (id == 2) {
        uint64_t utc = u64(p);
        stamp = boot_ms(m, u32(p + 8));
        if (utc >= MI_UNIX_MIN_US && utc < MI_UNIX_MAX_US && m->emit) {
            mi_event e;
            memset(&e, 0, sizeof(e)); e.kind = MI_CLOCK;
            e.sysid = sysid; e.compid = compid; e.epoch = m->epoch;
            e.boot_us = stamp; e.utc_us = utc;
            e.uncert_us = m->options.system_time_uncert_us;
            m->emit(m->context, &e);
        }
    } else if (id == 24) {
        uint32_t hacc = u32(p + 34), vacc = u32(p + 38);
        m->report.gps_fix = p[28];
        /* Report coordinates and their boot/receipt times belong to the same
         * GLOBAL_POSITION_INT sample. Raw GPS here supplies fix/accuracy only. */
        if (p[28] >= 3 && hacc && vacc && hacc != UINT32_MAX && vacc != UINT32_MAX) {
            m->gps_error_m = (float)(hypot((double)hacc, (double)vacc) * .001);
            m->gps_accuracy_ms = now;
        } else m->gps_error_m = NAN;
    } else if (id == 33) global_navigation(m, p, now);
    else if (id == 32) {
        /* LOCAL_POSITION_NED is estimator-local. Its origin is not guaranteed
         * to equal the configured geodetic origin; do not mix it with GPS ENU.
         * Timestamp still provides a boot clock watermark. */
        (void)boot_ms(m, u32(p));
    } else if (id == 251 || id == 252) {
        char name[TB_KEY_BYTES];
        stamp = boot_ms(m, u32(p));
        if (name_ok(name, p + 8))
            observation(m, stamp, now, name, "unknown", id == 251 ? (double)f32(p + 4) : (double)i32(p + 4));
        else ++m->report.invalid_value;
    } else if (id == 29 || id == 137 || id == 143) {
        const char *pressure = id == 29 ? "pressure" : id == 137 ? "pressure2" : "pressure3";
        const char *temperature = id == 29 ? "temperature" : id == 137 ? "temperature2" : "temperature3";
        stamp = boot_ms(m, u32(p));
        observation(m, stamp, now, pressure, "hPa", f32(p + 4));
        if (i16(p + 12) != INT16_MAX)
            observation(m, stamp, now, temperature, "degC", (double)i16(p + 12) * .01);
    } else if (id == 105) {
        uint16_t updated = u16(p + 60);
        if (boot_usec(m, u64(p), &stamp)) {
            char pressure[TB_KEY_BYTES], temperature[TB_KEY_BYTES];
            const char *pressure_key = "imu_pressure", *temperature_key = "imu_temperature";
            if (p[62]) {
                (void)snprintf(pressure, sizeof(pressure), "imu%u_pressure", (unsigned)p[62]);
                (void)snprintf(temperature, sizeof(temperature), "imu%u_temperature", (unsigned)p[62]);
                pressure_key = pressure; temperature_key = temperature;
            }
            if (updated & (UINT16_C(1) << 9)) observation(m, stamp, now, pressure_key, "hPa", f32(p + 44));
            if (updated & (UINT16_C(1) << 12)) observation(m, stamp, now, temperature_key, "degC", f32(p + 56));
        }
    }
    mi_tick(m, now);
}
static void resynchronize(mi_input *m)
{
    size_t j;
    for (j = 1; j < m->used; ++j) if (m->frame[j] == 0xfe || m->frame[j] == 0xfd) break;
    m->used -= j;
    if (m->used) memmove(m->frame, m->frame + j, m->used);
    m->needed = 0;
}
static void parse(mi_input *m, uint64_t now)
{
    for (;;) {
        const mi_message_info *info;
        size_t header, length, total;
        uint32_t id;
        uint16_t crc;
        uint8_t payload[255];
        int v2;
        if (!m->used) return;
        v2 = m->frame[0] == 0xfd; header = v2 ? 10u : 6u;
        if (m->used < header) return;
        length = m->frame[1];
        total = header + length + 2u + ((v2 && (m->frame[2] & 1u)) ? 13u : 0u);
        m->needed = total;
        if (v2 && (m->frame[2] & ~1u)) { ++m->report.unsupported_flags; resynchronize(m); continue; }
        id = v2 ? (uint32_t)m->frame[7] | ((uint32_t)m->frame[8] << 8) | ((uint32_t)m->frame[9] << 16) : m->frame[5];
        info = message_info(id);
        if (!info) { ++m->report.unknown_message; resynchronize(m); continue; }
        if ((!v2 && length != info->base_len) || (v2 && !length && info->base_len)) {
            ++m->report.bad_length; resynchronize(m); continue;
        }
        if (m->used < total) return;
        /* MAVLink2 truncates trailing zero bytes, including base fields.
         * Future extensions are accepted with the unchanged base CRC_EXTRA. */
        crc = checksum(m->frame + 1, header - 1u + length, info->crc);
        if (crc != u16(m->frame + header + length)) { ++m->report.bad_crc; resynchronize(m); continue; }
        if (v2 && (m->frame[2] & 1u) && !m->options.allow_signed_unverified) {
            ++m->report.signed_rejected;
        } else {
            memset(payload, 0, sizeof(payload)); memcpy(payload, m->frame + header, length);
            ++m->report.frames;
            packet(m, id, payload, m->frame[v2 ? 5 : 3], m->frame[v2 ? 6 : 4], m->frame[v2 ? 4 : 2], now);
        }
        m->used -= total;
        if (m->used) memmove(m->frame, m->frame + total, m->used);
        m->needed = 0;
        if (m->used && m->frame[0] != 0xfe && m->frame[0] != 0xfd) resynchronize(m);
    }
}
void mi_feed(mi_input *m, const uint8_t *bytes, size_t count, uint64_t now)
{
    size_t j;
    if (!m || (!bytes && count)) return;
    m->report.bytes += count;
    for (j = 0; j < count; ++j) {
        if (!m->used && bytes[j] != 0xfe && bytes[j] != 0xfd) continue;
        if (m->used == sizeof(m->frame)) resynchronize(m);
        m->frame[m->used++] = bytes[j];
        parse(m, now);
    }
}
size_t mi_heartbeat(uint8_t *out, size_t cap, uint8_t sysid, uint8_t compid, uint8_t sequence)
{
    uint16_t crc;
    if (!out || cap < 17 || !sysid || !compid) return 0;
    memset(out, 0, 17); out[0] = 0xfe; out[1] = 9; out[2] = sequence;
    out[3] = sysid; out[4] = compid;
    out[10] = 18; out[11] = 8; out[13] = 4; out[14] = 3;
    crc = checksum(out + 1, 14, 50); out[15] = (uint8_t)crc; out[16] = (uint8_t)(crc >> 8);
    return 17;
}