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
    /* Official ASLUAV.xml SENS_ATMOS; keep the generated common table intact. */
    static const mi_message_info sens_atmos = {8009, 144, 16, 16};
    size_t lo = 0, hi = sizeof(mi_common) / sizeof(mi_common[0]);
    if(id == sens_atmos.id) return &sens_atmos;
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
static void clear_position_state(mi_input *m)
{
    m->have_gps_fix = m->have_gps_anchor = m->have_global = 0;
    m->have_local_velocity = m->estimate_active = m->have_estimator_status = m->have_previous_velocity = 0;
    m->gps_anchor_us = m->gps_anchor_received_ms = m->gps_fix_boot_us = 0;
    m->have_gps_fix_clock = 0;
    m->navigation_segment = 0; m->absolute_invalid_active = 0;
    m->global_boot_us = m->global_received_ms = 0;
    m->local_velocity_us = m->local_velocity_received_ms = 0;
    m->estimate_boot_us = m->estimator_boot_us = m->estimator_received_ms = 0;
    m->estimator_flags = 0;
    m->report.gps_fix = m->report.gps_fix_fresh = m->report.have_gps_anchor = 0;
    m->report.have_navigation = 0;
    m->report.position_kind = WEATHER_POSITION_UNKNOWN;
    m->report.estimate_source = MI_ESTIMATE_NONE;
    m->report.estimation_age_us = UINT32_MAX;
    m->report.position_flags = MI_ESTGPS_NO_ANCHOR;
    m->report.navigation_error_m = NAN;
    m->report.gps_fix_received_ms = m->report.last_fix_boot_us = 0;
    m->report.navigation_received_ms = m->report.navigation_boot_us = 0;
    memset(m->report.navigation_v, 0, sizeof(m->report.navigation_v));
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
    clear_position_state(m);
    m->origin_set = !m->options.origin_auto;
    m->report.selected_system = m->options.target_system;
    m->report.selected_component = m->options.target_component;
}
void mi_link_reset(mi_input *m)
{
    if (m) {
        m->used = 0; m->needed = 0;
        m->report.no_navigation += m->pending_count + m->weather_pending_count;
        m->pending_count = m->weather_pending_count = m->nav_count = 0;
        m->gps_error_m = NAN; m->gps_accuracy_ms = 0;
        clear_position_state(m);
    }
}
void mi_get_report(const mi_input *m, mi_report *r)
{ if (m && r) { *r = m->report; r->pending = m->pending_count + m->weather_pending_count; } }

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
    m->report.weather_reboot_dropped += m->weather_pending_count;
    m->pending_count = 0; m->nav_count = 0;
    m->weather_pending_count = 0; m->weather_seq = 0;
    m->high_boot_us = m->wrap_ms = m->last_emitted_us = m->last_watermark_us = 0;
    m->last_boot_ms = m->next_seq = m->emitted_seq = 0; m->have_boot = m->have_emitted = 0;
    m->gps_error_m = NAN; m->gps_accuracy_ms = 0;
    clear_position_state(m);
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
static int gps_fresh(const mi_input *m, uint64_t now)
{
    return m->have_gps_fix && m->have_gps_fix_clock && m->have_boot &&
        m->report.gps_fix >= 3 && m->report.gps_fix <= 8 &&
        now >= m->report.gps_fix_received_ms &&
        now - m->report.gps_fix_received_ms <= MI_GPS_FIX_FRESH_MS &&
        (m->gps_fix_boot_us > m->high_boot_us ? m->gps_fix_boot_us - m->high_boot_us <= 1000 :
         m->high_boot_us - m->gps_fix_boot_us <= MI_GPS_FIX_FRESH_MS * 1000);
}
static int gps_fresh_at(const mi_input *m, uint64_t stamp, uint64_t now)
{
    return gps_fresh(m, now) &&
        (m->gps_fix_boot_us > stamp ? m->gps_fix_boot_us - stamp <= 1000 :
         stamp - m->gps_fix_boot_us <= MI_GPS_FIX_FRESH_MS * 1000);
}
static int estimator_status_fresh(const mi_input *m, uint64_t stamp, uint64_t now)
{
    /* Status never owns or resets the source clock. Against millisecond nav
     * time, allow only 1 ms of future timestamp quantization. */
    if (!m->have_estimator_status || now < m->estimator_received_ms ||
        now - m->estimator_received_ms > MI_EST_STATUS_FRESH_US / 1000) return 0;
    if (m->estimator_boot_us > stamp) return m->estimator_boot_us - stamp <= 1000;
    return stamp - m->estimator_boot_us <= MI_EST_STATUS_FRESH_US;
}
static int estimator_valid(const mi_input *m, uint64_t stamp, uint64_t now, uint16_t required)
{
    return estimator_status_fresh(m, stamp, now) &&
        !(m->estimator_flags & (1024u | 2048u | ((required & 6u) == 6u ? 128u : 0u))) &&
        (m->estimator_flags & required) == required;
}
static uint32_t estimation_age(const mi_input *m, uint64_t stamp)
{
    uint64_t age;
    if (!m->have_gps_anchor || stamp < m->gps_anchor_us) return UINT32_MAX;
    age = stamp - m->gps_anchor_us;
    return age < UINT32_MAX ? (uint32_t)age : UINT32_MAX;
}
static int anchor_in_range(const mi_input *m, uint64_t stamp, uint64_t now)
{
    return m->have_gps_anchor && stamp >= m->gps_anchor_us &&
        stamp - m->gps_anchor_us <= MI_EST_MAX_AGE_US &&
        now >= m->gps_anchor_received_ms &&
        now - m->gps_anchor_received_ms <= MI_EST_MAX_AGE_US / 1000;
}
static void add_nav(mi_input *m, const tb_nav *n, const mi_nav_info *info)
{
    unsigned j, slot;
    if (!isfinite(n->p[0]) || !isfinite(n->p[1]) || !isfinite(n->p[2]) ||
        !isfinite(n->v[0]) || !isfinite(n->v[1]) || !isfinite(n->v[2]) ||
        fabs(n->p[0]) > 1e9 || fabs(n->p[1]) > 1e9 || fabs(n->p[2]) > 1e9) {
        ++m->report.nav_rejected; return;
    }
    for (j = 0; j < m->nav_count; ++j) {
        if (m->nav[j].t_us == n->t_us) {
            /* A real EKF position wins over receiver-side integration. */
            if (m->nav_info[j].source == MI_ESTIMATE_EKF && info->source == MI_ESTIMATE_VELOCITY) return;
            m->nav[j] = *n; m->nav_info[j] = *info; return;
        }
        if (m->nav[j].t_us > n->t_us) break;
    }
    slot = j;
    if (m->nav_count == MI_NAV_CAP) {
        if (!slot) return;
        memmove(m->nav, m->nav + 1, (MI_NAV_CAP - 1u) * sizeof(m->nav[0]));
        memmove(m->nav_info, m->nav_info + 1, (MI_NAV_CAP - 1u) * sizeof(m->nav_info[0]));
        --slot; --m->nav_count;
    }
    for (j = m->nav_count; j > slot; --j) {
        m->nav[j] = m->nav[j - 1]; m->nav_info[j] = m->nav_info[j - 1];
    }
    m->nav[slot] = *n; m->nav_info[slot] = *info; ++m->nav_count;
}
static void invalidate_absolute_navigation(mi_input *m, uint64_t stamp, uint64_t now)
{
    unsigned j, out = 0;
    const int valid_velocity = estimator_valid(m, stamp, now, 2u | 4u);
    if (!m->absolute_invalid_active) {
        /* A segment barrier is fixed metadata, not a new coordinate. Keep
         * valid velocity-derived points during a continuous ABS outage. */
        if (m->navigation_segment == UINT32_MAX) { m->nav_count = 0; m->navigation_segment = 1; }
        else ++m->navigation_segment;
        m->absolute_invalid_active = 1;
    }
    for (j = 0; j < m->nav_count; ++j) {
        if (m->nav[j].t_us >= stamp &&
            (!valid_velocity || m->nav_info[j].source != MI_ESTIMATE_VELOCITY)) continue;
        m->nav[out] = m->nav[j]; m->nav_info[out++] = m->nav_info[j];
    }
    m->nav_count = out;
    for (j = 0; j < m->pending_count; ++j)
        if (m->pending[j].sample.t_us >= stamp) m->pending[j].aligned = 0;
    for (j = 0; j < m->weather_pending_count; ++j) {
        weather_sample *w = &m->weather_pending[j].sample;
        if (w->t_us < stamp) continue;
        w->has_position = 0; w->position_kind = WEATHER_POSITION_UNKNOWN;
        w->estimation_age_us = UINT32_MAX; w->pos_error_m = NAN;
        w->quality &= ~MI_QUALITY_ESTGPS; m->weather_pending[j].aligned = 0;
    }
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
static void aligned_metadata(tb_sample *s, mi_nav_info *out, const mi_nav_info *a, const mi_nav_info *b)
{
    mi_nav_info result = *a;
    if (b && (a->position_kind != WEATHER_POSITION_GPS || b->position_kind != WEATHER_POSITION_GPS)) {
        result.position_kind = WEATHER_POSITION_ESTGPS;
        result.have_anchor = 0;
        if (a->have_anchor && a->anchor_us <= s->t_us) { result.have_anchor = 1; result.anchor_us = a->anchor_us; }
        if (b->have_anchor && b->anchor_us <= s->t_us &&
            (!result.have_anchor || b->anchor_us < result.anchor_us)) {
            result.have_anchor = 1; result.anchor_us = b->anchor_us;
        }
    }
    if (result.position_kind == WEATHER_POSITION_ESTGPS) {
        s->quality |= MI_QUALITY_ESTGPS;
        s->pos_error_m = NAN; /* Integrating velocity supplies no error bound. */
    }
    if (out) *out = result;
}
static int align_sample(mi_input *m, tb_sample *s, mi_nav_info *meta)
{
    unsigned j;
    for (j = 0; j < m->nav_count; ++j) {
        if (m->nav[j].t_us == s->t_us) {
            unsigned k;
            for (k = 0; k < 3; ++k) { s->p[k] = m->nav[j].p[k]; s->v[k] = m->nav[j].v[k]; }
            s->pos_error_m = m->nav[j].pos_error_m; s->nav_age_us = 0;
            aligned_metadata(s, meta, m->nav_info + j, NULL);
            if (!isfinite(s->pos_error_m)) s->quality |= MI_QUALITY_POSITION_UNKNOWN;
            return 1;
        }
        if (j && m->nav[j - 1].t_us < s->t_us && m->nav[j].t_us > s->t_us) {
            if (m->nav_info[j - 1].segment != m->nav_info[j].segment) return 0;
            if (!tb_align(m->nav + j - 1, m->nav + j, s->t_us,
                          m->options.accel_bound_mps2, s)) return 0;
            aligned_metadata(s, meta, m->nav_info + j - 1, m->nav_info + j);
            if (!isfinite(s->pos_error_m)) s->quality |= MI_QUALITY_POSITION_UNKNOWN;
            return 1;
        }
    }
    return 0;
}
/* Weather retains observations even without navigation; unknown location is
 * explicit. Scan bounded 128 pending slots and 32 nav points; never extrapolate. */
static void emit_weather(mi_input *m, const weather_sample *sample)
{
    mi_event e;
    memset(&e,0,sizeof(e)); e.kind=MI_WEATHER_SAMPLE;
    e.sysid=m->report.selected_system; e.compid=m->report.selected_component;
    e.epoch=m->epoch; e.weather=*sample; e.boot_us=sample->t_us;
    ++m->report.observations;
    if(m->emit) m->emit(m->context,&e);
}
/* Official wire schemas only. Unknown HYG extension bytes are ignored.
 * SENS_ATMOS has one timestamped temperature/humidity pair, no sensor ID or
 * accuracy field. Its logical sensor is instance 0; message_id keeps it apart
 * from HYG instances if a source explicitly streams both representations. */
static void weather_observation(mi_input *m,const uint8_t *p,uint32_t id,uint64_t now)
{
    weather_sample s; uint64_t stamp=0; unsigned metric;
    double temperature,humidity;
    if(id==8009) {
        temperature=(double)f32(p+8); humidity=(double)f32(p+12);
        stamp=u64(p);
    } else {
        temperature=(double)i16(p)*.01; humidity=(double)u16(p+2)*.01;
    }
    if(!isfinite(temperature) || temperature < -273.15 ||
       !isfinite(humidity) || humidity < 0 || humidity > 100) {
        ++m->report.invalid_value; return;
    }
    /* A UNIX time cannot be joined to the FC boot clock by guessing. */
    if(stamp>=MI_UNIX_MIN_US) { ++m->report.no_clock; ++m->report.invalid_value; return; }
    if(m->weather_seq>UINT32_MAX-2u) reset_clock(m);
    memset(&s,0,sizeof(s)); s.message_id=id;
    s.sensor_instance=id==12920 ? p[4] : 0;
    s.measurement_variance=NAN; s.pos_error_m=NAN;
    s.time_uncert_us=UINT32_MAX; s.quality=MI_QUALITY_CLOCK_UNSIGNED;
    s.estimation_age_us=UINT32_MAX; s.position_kind=WEATHER_POSITION_UNKNOWN;
    if(stamp && boot_usec(m,stamp,&stamp)) { s.has_time=1; s.t_us=stamp; }
    for(metric=WEATHER_TEMPERATURE;metric<=WEATHER_HUMIDITY;++metric) {
        s.metric=(uint8_t)metric;
        s.value=metric==WEATHER_TEMPERATURE ? temperature+273.15 : humidity*.01;
        s.seq=++m->weather_seq;
        if(!s.has_time) emit_weather(m,&s);
        else if(m->weather_pending_count<WEATHER_CAPACITY) {
            unsigned slot=m->weather_pending_count++;
            m->weather_pending[slot].sample=s;
            m->weather_pending[slot].received_ms=now;
            m->weather_pending[slot].aligned=0;
        } else ++m->report.pending_overflow;
    }
}
static void flush_weather(mi_input *m,uint64_t now,int finishing)
{
    unsigned j=0;
    while(j<m->weather_pending_count) {
        weather_sample *s=&m->weather_pending[j].sample;
        uint64_t received=m->weather_pending[j].received_ms;
        if(!m->weather_pending[j].aligned) {
            tb_sample aligned; mi_nav_info meta;
            memset(&aligned,0,sizeof(aligned)); memset(&meta,0,sizeof(meta)); aligned.t_us=s->t_us;
            if(align_sample(m,&aligned,&meta)) {
                memcpy(s->p,aligned.p,sizeof(s->p)); memcpy(s->v,aligned.v,sizeof(s->v));
                s->pos_error_m=aligned.pos_error_m; s->has_position=1;
                s->position_kind=meta.position_kind;
                s->estimation_age_us=meta.position_kind==WEATHER_POSITION_GPS ? 0 :
                    meta.have_anchor && s->t_us>=meta.anchor_us && s->t_us-meta.anchor_us<UINT32_MAX ?
                    (uint32_t)(s->t_us-meta.anchor_us) : UINT32_MAX;
                if(meta.position_kind==WEATHER_POSITION_ESTGPS) s->quality|=MI_QUALITY_ESTGPS;
                m->weather_pending[j].aligned=1;
            }
        }
        if(finishing || (now>=received && now-received>=200)) {
            if(!s->has_position) ++m->report.no_navigation;
            emit_weather(m,s);
            m->weather_pending[j]=m->weather_pending[--m->weather_pending_count];
        } else ++j;
    }
}
static void flush_pending(mi_input *m, uint64_t watermark, uint64_t now, int finishing)
{
    unsigned cache;
    /* Capture an actual bracketing solution as soon as it exists. Navigation
     * can be much faster than the 200ms reorder horizon; retaining coordinates
     * in the pending observation prevents a 32-entry nav ring from erasing
     * already available evidence before emission. No extrapolation is used. */
    for (cache = 0; cache < m->pending_count; ++cache)
        if (!m->pending[cache].aligned && align_sample(m, &m->pending[cache].sample, NULL))
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
    m->report.pending = m->pending_count + m->weather_pending_count;
}
static void refresh_navigation(mi_input *m, uint64_t now)
{
    m->report.gps_fix_fresh = (uint8_t)gps_fresh(m, now);
    m->report.have_gps_anchor = m->have_gps_anchor;
    m->report.last_fix_boot_us = m->gps_anchor_us;
    if (m->report.position_kind == WEATHER_POSITION_GPS && !m->report.gps_fix_fresh) {
        m->report.position_kind = WEATHER_POSITION_ESTGPS;
        m->report.navigation_error_m = NAN;
        m->report.estimation_age_us = estimation_age(m, m->report.navigation_boot_us);
    }
    if (m->report.estimate_source == MI_ESTIMATE_VELOCITY) {
        if (!anchor_in_range(m, m->high_boot_us, now)) {
            m->estimate_active = 0;
            m->report.position_flags |= m->have_gps_anchor ? MI_ESTGPS_EXPIRED : MI_ESTGPS_NO_ANCHOR;
        }
        if (!estimator_valid(m, m->estimator_boot_us > m->high_boot_us ? m->estimator_boot_us : m->high_boot_us,
                             now, 2u | 4u)) {
            m->estimate_active = 0; m->report.position_flags |= MI_ESTGPS_STATUS_INVALID;
        }
        if (!m->have_local_velocity || now < m->local_velocity_received_ms ||
            now - m->local_velocity_received_ms > MI_EST_VELOCITY_GAP_US / 1000) {
            m->estimate_active = 0; m->report.position_flags |= MI_ESTGPS_VELOCITY_GAP;
        }
        if (!m->estimate_active) {
            m->report.have_navigation = 0;
            m->report.position_kind = WEATHER_POSITION_UNKNOWN;
            m->report.navigation_error_m = NAN;
        }
    } else if (m->have_global &&
               (now < m->global_received_ms || now - m->global_received_ms > MI_GLOBAL_FRESH_MS ||
                (estimator_status_fresh(m, m->high_boot_us, now) &&
                 !estimator_valid(m, m->high_boot_us, now, 16u | 32u)))) {
        m->report.have_navigation = 0;
        m->report.position_kind = WEATHER_POSITION_UNKNOWN;
    }
}
void mi_tick(mi_input *m, uint64_t now)
{
    uint64_t watermark;
    if (!m) return;
    refresh_navigation(m, now);
    watermark = m->high_boot_us > MI_REORDER_US ? m->high_boot_us - MI_REORDER_US : 0;
    flush_weather(m,now,0);
    flush_pending(m, watermark, now, 0);
}
void mi_finish(mi_input *m, uint64_t now)
{
    if (m) { flush_weather(m,now,1); flush_pending(m, m->high_boot_us, now, 1); }
}
static void report_navigation(mi_input *m, const tb_nav *nav, const mi_nav_info *info,
                              double lat, double lon, double altitude, uint64_t now)
{
    if (!m->report.navigation_boot_us || nav->t_us >= m->report.navigation_boot_us) {
        m->report.lat = lat; m->report.lon = lon; m->report.altitude_msl_m = altitude;
        m->report.have_navigation = 1;
        m->report.navigation_received_ms = now;
        m->report.navigation_boot_us = nav->t_us;
        m->report.position_kind = info->position_kind;
        m->report.estimate_source = info->source;
        m->report.estimation_age_us = info->position_kind == WEATHER_POSITION_GPS ? 0 : estimation_age(m, nav->t_us);
        m->report.navigation_error_m = nav->pos_error_m;
        memcpy(m->report.navigation_v, nav->v, sizeof(nav->v));
    }
}
static void global_navigation(mi_input *m, const uint8_t *p, uint64_t now)
{
    tb_nav nav; mi_nav_info info;
    double lat = (double)i32(p + 4) * 1e-7, lon = (double)i32(p + 8) * 1e-7;
    double altitude = (double)i32(p + 12) * .001;
    double rad, sine, scale, meridian, prime, dlon;
    int trusted;
    memset(&nav, 0, sizeof(nav)); memset(&info, 0, sizeof(info));
    nav.t_us = boot_ms(m, u32(p));
    if (!isfinite(lat) || lat < -90 || lat > 90 || lon < -180 || lon > 180) {
        ++m->report.nav_rejected; return;
    }
    /* Reject known-invalid absolute data before it can set an automatic
     * origin. A recovered point may not bridge the invalid interval later. */
    if (estimator_status_fresh(m, nav.t_us, now) && !estimator_valid(m, nav.t_us, now, 16u | 32u)) {
        ++m->report.nav_rejected; m->report.position_flags |= MI_ESTGPS_STATUS_INVALID;
        invalidate_absolute_navigation(m, nav.t_us, now);
        m->have_global = 0;
        if (m->report.estimate_source == MI_ESTIMATE_EKF) {
            m->report.have_navigation = 0; m->report.position_kind = WEATHER_POSITION_UNKNOWN;
        }
        return;
    }
    m->absolute_invalid_active = 0;
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
    /* Regional WGS84 radii projection. Altitude stays MSL; never call this an
     * ellipsoidal height or assign the receiver an invented error bound. */
    nav.p[0] = dlon * (MI_PI / 180.0) * prime * cos(rad);
    nav.p[1] = (lat - m->options.origin_lat) * (MI_PI / 180.0) * meridian;
    nav.p[2] = altitude - m->options.origin_alt;
    nav.v[0] = (float)i16(p + 22) * .01f;
    nav.v[1] = (float)i16(p + 20) * .01f;
    nav.v[2] = (float)i16(p + 24) * -.01f;
    nav.pos_error_m = NAN;
    trusted = nav.t_us && gps_fresh_at(m, nav.t_us, now) && estimator_valid(m, nav.t_us, now, 16u | 32u);
    info.position_kind = trusted ? WEATHER_POSITION_GPS : WEATHER_POSITION_ESTGPS;
    info.source = MI_ESTIMATE_EKF; info.segment = m->navigation_segment;
    if (trusted && (!m->have_gps_anchor || nav.t_us > m->gps_anchor_us)) {
        m->gps_anchor_us = nav.t_us; m->gps_anchor_received_ms = now; m->have_gps_anchor = 1;
    }
    info.have_anchor = m->have_gps_anchor; info.anchor_us = m->gps_anchor_us;
    if (trusted && isfinite(m->gps_error_m) && now >= m->gps_accuracy_ms && now - m->gps_accuracy_ms <= 2000) {
        double distance = hypot(nav.p[0], nav.p[1]);
        double projection_error = distance * distance / 6000000.0;
        nav.pos_error_m = (float)((double)m->gps_error_m + projection_error);
    }
    if (!m->have_global || nav.t_us >= m->global_boot_us) {
        m->have_global = 1; m->global_boot_us = nav.t_us; m->global_received_ms = now;
        m->meridian_m = meridian; m->east_radius_m = prime * cos(rad);
        memcpy(m->estimate_p, nav.p, sizeof(nav.p)); m->estimate_boot_us = nav.t_us;
        m->estimate_active = (uint8_t)(anchor_in_range(m, nav.t_us, now) &&
                                      estimator_valid(m, nav.t_us, now, 2u | 4u));
        /* GPI may arrive after a newer LOCAL_POSITION_NED. If the last two
         * validated velocities bracket its timestamp, reconstruct only that
         * measured interval. Do not extrapolate a missing earlier velocity. */
        if (m->estimate_active && m->have_local_velocity && m->local_velocity_us > nav.t_us) {
            if (m->have_previous_velocity && m->previous_velocity_us <= nav.t_us &&
                m->local_velocity_us - m->previous_velocity_us <= MI_EST_VELOCITY_GAP_US &&
                now >= m->previous_velocity_received_ms &&
                now - m->previous_velocity_received_ms <= MI_EST_VELOCITY_GAP_US / 1000 &&
                estimator_valid(m, m->local_velocity_us, now, 2u | 4u)) {
                const double fraction = (double)(nav.t_us - m->previous_velocity_us) /
                                        (double)(m->local_velocity_us - m->previous_velocity_us);
                const double dt = (double)(m->local_velocity_us - nav.t_us) * 1e-6;
                unsigned k;
                for (k = 0; k < 3; ++k) {
                    const double initial_v = m->previous_velocity[k] +
                        fraction * ((double)m->local_velocity[k] - m->previous_velocity[k]);
                    m->estimate_p[k] += .5 * (initial_v + m->local_velocity[k]) * dt;
                }
                m->estimate_boot_us = m->local_velocity_us;
            } else m->estimate_active = 0;
        }
        m->report.position_flags = m->have_gps_anchor ? 0 : MI_ESTGPS_NO_ANCHOR;
        if (!estimator_valid(m, nav.t_us, now, 16u | 32u)) m->report.position_flags |= MI_ESTGPS_STATUS_INVALID;
    }
    report_navigation(m, &nav, &info, lat, lon, altitude, now);
    add_nav(m, &nav, &info);
}
static void local_navigation(mi_input *m, const uint8_t *p, uint64_t now)
{
    const uint64_t stamp = boot_ms(m, u32(p));
    float velocity[3]; unsigned j;
    velocity[0] = f32(p + 20); /* east = LOCAL_POSITION_NED.vy */
    velocity[1] = f32(p + 16); /* north = vx */
    velocity[2] = -f32(p + 24); /* up = -vz */
    if (!estimator_valid(m, stamp, now, 2u | 4u)) {
        m->have_local_velocity = m->have_previous_velocity = m->estimate_active = 0;
        m->report.position_flags |= MI_ESTGPS_STATUS_INVALID; return;
    }
    if (!isfinite(velocity[0]) || !isfinite(velocity[1]) || !isfinite(velocity[2]) ||
        hypot(hypot((double)velocity[0], velocity[1]), velocity[2]) > MI_EST_VELOCITY_MAX_MPS) {
        m->have_local_velocity = m->have_previous_velocity = m->estimate_active = 0;
        m->report.position_flags |= MI_ESTGPS_VELOCITY_INVALID;
        ++m->report.nav_rejected; return;
    }
    /* Duplicate/reordered velocity cannot move an already integrated solution
     * or extend freshness. A missing interval cannot be filled with zero speed. */
    if (m->have_local_velocity && stamp <= m->local_velocity_us) return;
    if (m->estimate_active && stamp > m->estimate_boot_us) {
        if (!m->have_local_velocity || m->local_velocity_us > m->estimate_boot_us ||
            stamp - m->local_velocity_us > MI_EST_VELOCITY_GAP_US ||
            now < m->local_velocity_received_ms || now - m->local_velocity_received_ms > MI_EST_VELOCITY_GAP_US / 1000) {
            m->estimate_active = 0; m->report.position_flags |= MI_ESTGPS_VELOCITY_GAP;
        } else {
            const double fraction = (double)(m->estimate_boot_us - m->local_velocity_us) /
                                    (double)(stamp - m->local_velocity_us);
            const double dt = (double)(stamp - m->estimate_boot_us) * 1e-6;
            for (j = 0; j < 3; ++j) {
                const double initial_v = m->local_velocity[j] + fraction * ((double)velocity[j] - m->local_velocity[j]);
                m->estimate_p[j] += .5 * (initial_v + velocity[j]) * dt;
            }
            m->estimate_boot_us = stamp;
        }
    }
    if (m->have_local_velocity) {
        memcpy(m->previous_velocity, m->local_velocity, sizeof(m->local_velocity));
        m->previous_velocity_us = m->local_velocity_us;
        m->previous_velocity_received_ms = m->local_velocity_received_ms;
        m->have_previous_velocity = 1;
    }
    memcpy(m->local_velocity, velocity, sizeof(velocity));
    m->local_velocity_us = stamp; m->local_velocity_received_ms = now; m->have_local_velocity = 1;
    if (!m->have_gps_anchor) { m->report.position_flags |= MI_ESTGPS_NO_ANCHOR; return; }
    if (!anchor_in_range(m, stamp, now)) {
        m->estimate_active = 0; m->report.position_flags |= MI_ESTGPS_EXPIRED; return;
    }
    if (!m->estimate_active || stamp != m->estimate_boot_us) return;
    /* Prefer a fresh GLOBAL_POSITION_INT position. Continue the accumulator in
     * the background, but do not publish its displacement on top of EKF data. */
    if (m->have_global && now >= m->global_received_ms && now - m->global_received_ms <= MI_GLOBAL_FRESH_MS) return;
    if (!isfinite(m->east_radius_m) || fabs(m->east_radius_m) < 1.0 || !(m->meridian_m > 0)) {
        m->estimate_active = 0; m->report.position_flags |= MI_ESTGPS_VELOCITY_INVALID; return;
    }
    {
        tb_nav nav; mi_nav_info info; double lat, lon, altitude;
        memset(&nav, 0, sizeof(nav)); memset(&info, 0, sizeof(info)); nav.t_us = stamp;
        memcpy(nav.p, m->estimate_p, sizeof(nav.p)); memcpy(nav.v, velocity, sizeof(nav.v)); nav.pos_error_m = NAN;
        lat = m->options.origin_lat + nav.p[1] / m->meridian_m * (180.0 / MI_PI);
        lon = m->options.origin_lon + nav.p[0] / m->east_radius_m * (180.0 / MI_PI);
        lon = fmod(lon + 180.0, 360.0); if (lon < 0) lon += 360.0; lon -= 180.0;
        altitude = m->options.origin_alt + nav.p[2];
        if (!isfinite(lat) || fabs(lat) > 90 || !isfinite(lon) || !isfinite(altitude)) {
            m->estimate_active = 0; ++m->report.nav_rejected; return;
        }
        info.position_kind = WEATHER_POSITION_ESTGPS; info.source = MI_ESTIMATE_VELOCITY;
        info.segment = m->navigation_segment;
        info.have_anchor = 1; info.anchor_us = m->gps_anchor_us;
        m->report.position_flags = 0;
        report_navigation(m, &nav, &info, lat, lon, altitude, now);
        add_nav(m, &nav, &info);
    }
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
        if (id == 251 || id == 252 || id == 29 || id == 137 || id == 143 || id == 105 || id == 12920 || id == 8009)
            ++m->report.wrong_clock_component;
        return;
    }
    if(id==12920 || id==8009) weather_observation(m,p,id,now);
    else if (id == 2) {
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
        const uint64_t gps_stamp = u64(p);
        m->report.gps_fix = p[28]; m->have_gps_fix = 1;
        m->report.gps_fix_received_ms = now;
        m->have_gps_fix_clock = gps_stamp && gps_stamp < MI_UNIX_MIN_US;
        if (m->have_gps_fix_clock && m->have_boot &&
            ((gps_stamp > m->high_boot_us && gps_stamp - m->high_boot_us > MI_GPS_FIX_FRESH_MS * 1000) ||
             (gps_stamp <= m->high_boot_us && m->high_boot_us - gps_stamp > MI_GPS_FIX_FRESH_MS * 1000)))
            m->have_gps_fix_clock = 0;
        m->gps_fix_boot_us = m->have_gps_fix_clock ? gps_stamp : 0;
        /* Report coordinates and their boot/receipt times belong to the same
         * GLOBAL_POSITION_INT sample. Raw GPS here supplies fix/accuracy only. */
        if (p[28] >= 3 && p[28] <= 8 && hacc && vacc && hacc != UINT32_MAX && vacc != UINT32_MAX) {
            m->gps_error_m = (float)(hypot((double)hacc, (double)vacc) * .001);
            m->gps_accuracy_ms = now;
        } else m->gps_error_m = NAN;
    } else if (id == 33) global_navigation(m, p, now);
    else if (id == 32) local_navigation(m, p, now);
    else if (id == 230) {
        const uint64_t time_us = u64(p);
        int clock_compatible = time_us && time_us < MI_UNIX_MIN_US;
        /* Delayed or future status must never call boot_usec/reset_clock:
         * only navigation/measurement clock owners define a source epoch. */
        if (clock_compatible && m->have_boot &&
            ((time_us > m->high_boot_us && time_us - m->high_boot_us > MI_EST_STATUS_FRESH_US) ||
             (time_us <= m->high_boot_us && m->high_boot_us - time_us > MI_EST_STATUS_FRESH_US)))
            clock_compatible = 0;
        if (!clock_compatible) {
            if (time_us >= MI_UNIX_MIN_US) ++m->report.no_clock;
            else ++m->report.nav_rejected;
        } else {
            if (m->have_estimator_status && m->have_boot &&
                ((m->estimator_boot_us > m->high_boot_us && m->estimator_boot_us - m->high_boot_us > MI_EST_STATUS_FRESH_US) ||
                 (m->estimator_boot_us <= m->high_boot_us && m->high_boot_us - m->estimator_boot_us > MI_EST_STATUS_FRESH_US)))
                m->have_estimator_status = 0;
            if (!m->have_estimator_status || time_us >= m->estimator_boot_us) {
                m->estimator_boot_us = time_us; m->estimator_received_ms = now;
                m->estimator_flags = u16(p + 40); m->have_estimator_status = 1;
                if ((m->estimator_flags & (128u | 1024u | 2048u)) || (m->estimator_flags & 6u) != 6u) {
                    m->estimate_active = m->have_local_velocity = m->have_previous_velocity = 0;
                    m->report.position_flags |= MI_ESTGPS_STATUS_INVALID;
                }
            }
        }
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