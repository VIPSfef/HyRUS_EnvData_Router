#include "mavlink_rates.h"
#include "qgc_bridge.h"
#include <limits.h>
#include <string.h>
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Waddress-of-packed-member"
#endif
#include "vendor/mavlink/mavlink_registry.h"
#include "vendor/mavlink/protocol.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

enum { MR_OWN_SYSTEM = 255, MR_OWN_COMPONENT = 191,
       MR_SET_INTERVAL = 511, MR_COMMAND_LONG = 76, MR_COMMAND_INT = 75,
       MR_COMMAND_ACK = 77, MR_REQUEST_STREAM = 66, MR_RADIO_STATUS = 109,
       MR_ACK_ACCEPTED = 0, MR_ACK_TEMPORARY = 1, MR_ACK_PROGRESS = 5,
       MR_UART_BITS_PER_BYTE = 10, MR_CEILING_PERCENT = 70,
       MR_INITIAL_PERCENT = 125, MR_GROW_PERCENT = 110, MR_SHRINK_PERCENT = 80,
       MR_UTILIZATION_PERCENT = 65, MR_LOSS_PERCENT = 5,
       MR_RADIO_LOW_PERCENT = 25, MR_RADIO_PROBE_PERCENT = 50,
       MR_SLOW_GROW_PERCENT = 105, MR_PROBE_GAIN_PERCENT = 2,
       MR_REARM_GAIN_PERCENT = 20, MR_HYSTERESIS_PERCENT = 10,
       MR_SCALE_MIN_MILLI = 250, MR_SCALE_QUANTUM_MILLI = 50,
       MR_SCALE_AGREEMENT_PERCENT = 25, MR_SCALE_MIN_STREAMS = 3,
       MR_MEASUREMENT_WEIGHT = 4,
       MR_HEARTBEAT_WIRE_BYTES = 34, MR_MILLIHZ = 1000,
       MR_US_PER_SECOND = 1000000, MR_SIGNED_OVERHEAD = 25,
       MR_INTERVAL_QUANTUM_US = 10000, MR_FALLBACK_MILLIHZ = 200,
       MR_COMMAND_PARAMETERS = 7, MR_PARAMETER_BYTES = 4,
       MR_AUTOPILOT_INVALID = 8, MR_TYPE_GCS = 6 };
typedef enum { MR_PRIMARY, MR_SECONDARY, MR_OTHER } mr_priority;
typedef enum { MR_ALWAYS, MR_SEEN, MR_WEATHER_HYG, MR_WEATHER_SENS,
               MR_AIR_VFR, MR_AIR_SENSOR } mr_activation;
typedef struct { uint32_t id; unsigned cap_hz; mr_priority priority; mr_activation activation; } mr_definition;
typedef enum { MR_GPS, MR_GLOBAL_POSITION, MR_HYGROMETER, MR_SENS_TEMP,
               MR_VFR_HUD, MR_AIRSPEED, MR_ESTIMATOR, MR_ATTITUDE,
               MR_QUATERNION } mr_stream_index;
/* This is a periodic-telemetry allowlist. Commands, parameters, mission traffic,
 * events and unknown messages are never converted to periodic streams. One
 * temperature/humidity path and one airspeed path are requested, not both. */
static const mr_definition definitions[] = {
    {24,10,MR_PRIMARY,MR_ALWAYS}, {33,20,MR_PRIMARY,MR_ALWAYS},
    {12920,10,MR_PRIMARY,MR_WEATHER_HYG}, {8009,10,MR_PRIMARY,MR_WEATHER_SENS},
    {74,10,MR_PRIMARY,MR_AIR_VFR}, {295,10,MR_PRIMARY,MR_AIR_SENSOR},
    {230,5,MR_PRIMARY,MR_ALWAYS},
    {30,2,MR_SECONDARY,MR_SEEN}, {31,2,MR_SECONDARY,MR_SEEN},
    {1,1,MR_OTHER,MR_SEEN}, {2,1,MR_OTHER,MR_SEEN},
    {25,1,MR_OTHER,MR_SEEN}, {26,1,MR_OTHER,MR_SEEN},
    {27,1,MR_OTHER,MR_SEEN}, {29,1,MR_OTHER,MR_SEEN},
    {32,1,MR_OTHER,MR_SEEN}, {36,1,MR_OTHER,MR_SEEN},
    {62,1,MR_OTHER,MR_SEEN}, {65,1,MR_OTHER,MR_SEEN},
    {85,1,MR_OTHER,MR_SEEN}, {87,1,MR_OTHER,MR_SEEN},
    {105,1,MR_OTHER,MR_SEEN}, {124,1,MR_OTHER,MR_SEEN},
    {132,1,MR_OTHER,MR_SEEN}, {137,1,MR_OTHER,MR_SEEN},
    {143,1,MR_OTHER,MR_SEEN}, {147,1,MR_OTHER,MR_SEEN},
    {241,1,MR_OTHER,MR_SEEN}, {245,1,MR_OTHER,MR_SEEN},
    {83,1,MR_OTHER,MR_SEEN}, {141,1,MR_OTHER,MR_SEEN}
};
#define MR_STREAMS (sizeof(definitions)/sizeof(definitions[0]))
typedef char mr_stream_table_fits[(MR_STREAMS<=MR_STREAM_CAP) ? 1 : -1];
typedef struct { const uint8_t *payload; unsigned len, sysid, compid; uint32_t id; int signed_frame; } mr_wire;

static int view(const uint8_t *frame,size_t length,mr_wire *out) {
    size_t header;
    if(!frame || length<8 || (frame[0]!=0xfd && frame[0]!=0xfe)) return 0;
    header=frame[0]==0xfd ? 10u : 6u;
    if(header==10 && (frame[2]&~1u)) return 0; /* No extended addressing here. */
    if(length<header+frame[1]+2u) return 0;
    out->payload=frame+header; out->len=frame[1];
    out->signed_frame=header==10 && (frame[2]&1u);
    if(header==10) { out->sysid=frame[5]; out->compid=frame[6]; }
    else { out->sysid=frame[3]; out->compid=frame[4]; }
    out->id=frame[header-1];
    if(header==10) out->id=(uint32_t)frame[7]|(uint32_t)frame[8]<<8|(uint32_t)frame[9]<<16;
    return 1;
}
static unsigned u16(const uint8_t *p) { return (unsigned)p[0]|(unsigned)p[1]<<8; }
static unsigned bytes_per_message(unsigned index) {
    const mavlink_msg_entry_t *entry=mavlink_get_msg_entry(definitions[index].id);
    return entry ? entry->max_msg_len+MR_SIGNED_OVERHEAD : 0;
}
static unsigned command_reserve_bps(void) {
    const mavlink_msg_entry_t *command=mavlink_get_msg_entry(MR_COMMAND_LONG);
    const mavlink_msg_entry_t *ack=mavlink_get_msg_entry(MR_COMMAND_ACK);
    unsigned bytes=command->max_msg_len+ack->max_msg_len+2u*MAVLINK_NUM_NON_PAYLOAD_BYTES;
    return (bytes*1000u+MR_PACE_MS-1u)/MR_PACE_MS;
}
static unsigned minimum(unsigned a,unsigned b) { return a<b ? a : b; }
static unsigned scaled(unsigned value,unsigned percent) { return (unsigned)((uint64_t)value*percent/100u); }
const char *mr_state_name(mr_state state) {
    static const char *const names[]={"OFF","WAIT_LINK","OBSERVING","APPLYING","MONITORING","CONGESTED","WAIT_GCS","PROBING","HOLD"};
    return (unsigned)state<sizeof(names)/sizeof(names[0]) ? names[state] : "UNKNOWN";
}
void mr_init(mr_controller *c,int enabled,unsigned baud) {
    memset(c,0,sizeof(*c)); c->enabled=(uint8_t)(enabled!=0);
    c->report.state=enabled ? MR_WAIT_LINK : MR_DISABLED;
    c->report.radio_txbuf=-1;
    c->report.rate_scale_milli=MR_MILLIHZ;
    c->uart_ceiling_bps=scaled(baud/MR_UART_BITS_PER_BYTE,MR_CEILING_PERCENT);
}
static int active(const mr_controller *c,unsigned i) {
    if(c->stream[i].blocked) return 0;
    switch(definitions[i].activation) {
    case MR_ALWAYS: return 1;
    case MR_SEEN: return c->stream[i].observed_millihz>definitions[i].cap_hz*MR_MILLIHZ ||
                        c->stream[i].applied_us || c->stream[i].desired_us;
    case MR_WEATHER_HYG: return c->weather_choice==MR_WEATHER_HYG;
    case MR_WEATHER_SENS: return c->weather_choice==MR_WEATHER_SENS;
    case MR_AIR_VFR: return c->airspeed_choice==MR_AIR_VFR;
    case MR_AIR_SENSOR: return c->airspeed_choice==MR_AIR_SENSOR;
    }
    return 0;
}
static void choose_sensor_paths(mr_controller *c) {
    if(!c->weather_choice) c->weather_choice=c->stream[MR_SENS_TEMP].observed && !c->stream[MR_HYGROMETER].observed ? MR_WEATHER_SENS : MR_WEATHER_HYG;
    if(c->weather_choice==MR_WEATHER_HYG && c->stream[MR_HYGROMETER].blocked && c->stream[MR_SENS_TEMP].observed) c->weather_choice=MR_WEATHER_SENS;
    if(!c->airspeed_choice) c->airspeed_choice=c->stream[MR_AIRSPEED].observed ? MR_AIR_SENSOR : MR_AIR_VFR;
    if(c->airspeed_choice==MR_AIR_SENSOR && c->stream[MR_AIRSPEED].blocked) c->airspeed_choice=MR_AIR_VFR;
}
/* PX4 may scale all periodic streams below their accepted requested rates.
 * Predict actual frequency before pricing it against observed wire bytes;
 * only the increment above that observation needs maximum packet sizes. */
static unsigned stream_cost(const mr_controller *c,unsigned index,unsigned millihz) {
    const mr_stream *s=&c->stream[index];
    millihz=(unsigned)(((uint64_t)millihz*c->report.rate_scale_milli+MR_MILLIHZ-1u)/MR_MILLIHZ);
    if(s->average_millihz && millihz<=s->average_millihz)
        return (unsigned)(((uint64_t)s->average_bps*millihz+s->average_millihz-1u)/s->average_millihz);
    return s->average_bps+(bytes_per_message(index)*(millihz-s->average_millihz)+MR_MILLIHZ-1u)/MR_MILLIHZ;
}
static unsigned plan_cost(const mr_controller *c,const unsigned *rate) {
    unsigned i,cost=0;
    for(i=0;i<MR_STREAMS;++i) if(rate[i]) cost+=stream_cost(c,i,rate[i]);
    return cost;
}
/* Raise one priority group by a common fraction of its remaining headroom.
 * The measured/max-size piecewise cost is monotonic, so a bounded binary search
 * covers both baseline allocation and optional higher rates without duplication. */
static void raise_group(const mr_controller *c,unsigned *rate,unsigned mask,unsigned goal,unsigned available) {
    unsigned low=0,high=MR_MILLIHZ,i,trial[MR_STREAM_CAP],target[MR_STREAM_CAP];
    for(i=0;i<MR_STREAMS;++i) {
        unsigned cap=definitions[i].cap_hz*MR_MILLIHZ, requested=goal;
        if(goal && definitions[i].priority==MR_PRIMARY)
            requested=(unsigned)(((uint64_t)goal*MR_MILLIHZ+c->report.rate_scale_milli-1u)/c->report.rate_scale_milli);
        target[i]=rate[i] && (mask&(1u<<definitions[i].priority)) ? (goal ? minimum(requested,cap) : cap) : rate[i];
    }
    while(low<high) {
        unsigned fraction=(low+high+1u)/2u;
        for(i=0;i<MR_STREAMS;++i) trial[i]=rate[i]+(target[i]-rate[i])*fraction/MR_MILLIHZ;
        if(plan_cost(c,trial)<=available) low=fraction;
        else high=fraction-1u;
    }
    for(i=0;i<MR_STREAMS;++i) rate[i]+=(target[i]-rate[i])*low/MR_MILLIHZ;
}
static unsigned interval_rate(uint32_t interval) {
    return interval ? (unsigned)((UINT64_C(1000000000)+interval-1u)/interval) : 0;
}
/* Essential 1Hz comes before raising any background stream above its recovery
 * floor. Extra primary rates then get priority over secondary headroom. */
static void plan(mr_controller *c,unsigned unmanaged_bps) {
    unsigned rate[MR_STREAM_CAP]={0},i,available,baseline,growth_budget;
    int model_changed=c->planned_budget_bps!=c->report.budget_bps ||
        c->planned_rate_scale_milli!=c->report.rate_scale_milli;
    unsigned reserved=unmanaged_bps+MR_HEARTBEAT_WIRE_BYTES+command_reserve_bps();
    choose_sensor_paths(c);
    for(i=0;i<MR_STREAMS;++i) if(!active(c,i)) reserved+=c->stream[i].average_bps;
    available=c->report.budget_bps>reserved ? c->report.budget_bps-reserved : 0;
    for(i=0;i<MR_STREAMS;++i) if(active(c,i)) rate[i]=MR_FALLBACK_MILLIHZ;
    raise_group(c,rate,1u<<MR_PRIMARY,MR_MILLIHZ,available);
    for(i=0;i<MR_STREAMS;++i) if(rate[i] && definitions[i].priority==MR_PRIMARY &&
        (uint64_t)rate[i]*c->report.rate_scale_milli<MR_MILLIHZ*MR_MILLIHZ) c->congested=1;
    raise_group(c,rate,(1u<<MR_SECONDARY)|(1u<<MR_OTHER),MR_MILLIHZ,available);
    baseline=plan_cost(c,rate);
    growth_budget=available;
    if(available>baseline) growth_budget-=minimum(scaled(available,MR_HYSTERESIS_PERCENT),available-baseline);
    raise_group(c,rate,1u<<MR_PRIMARY,0,growth_budget);
    raise_group(c,rate,1u<<MR_SECONDARY,0,growth_budget);
    for(i=0;i<MR_STREAMS;++i) {
        uint32_t interval=rate[i] ? (uint32_t)((UINT64_C(1000000000)+rate[i]-1u)/rate[i]) : 0;
        uint32_t old=c->stream[i].desired_us;
        if(interval) interval=(interval+MR_INTERVAL_QUANTUM_US-1u)/MR_INTERVAL_QUANTUM_US*MR_INTERVAL_QUANTUM_US;
        rate[i]=interval_rate(interval);
        /* Measurement noise may hold the previous rate, but an explicit trial
         * or rollback must really change the requested profile to be measurable. */
        if(!c->congested && !model_changed && old && interval &&
           !(definitions[i].priority==MR_PRIMARY && old>c->report.rate_scale_milli*MR_MILLIHZ &&
             interval<=c->report.rate_scale_milli*MR_MILLIHZ) &&
           old>=MR_US_PER_SECOND/definitions[i].cap_hz) {
            uint32_t delta=old>interval ? old-interval : interval-old;
            if((uint64_t)delta*100u<=(uint64_t)old*MR_HYSTERESIS_PERCENT) {
                unsigned proposed=rate[i]; rate[i]=interval_rate(old);
                if(plan_cost(c,rate)<=available) interval=old;
                else rate[i]=proposed;
            }
        }
        c->stream[i].desired_us=interval;
    }
    c->report.planned_bps=plan_cost(c,rate);
    c->planned_budget_bps=c->report.budget_bps;
    c->planned_rate_scale_milli=c->report.rate_scale_milli;
}
static void reset_scale_sample(mr_stream *stream) {
    stream->scale_ms=stream->scale_frames=stream->scale_at_ms=0;
    stream->scale_milli=0;
}
static void finish_command(mr_controller *c,uint64_t now,int accepted) {
    mr_stream *stream=&c->stream[c->pending-1u];
    if(accepted) {
        if(stream->applied_us!=c->pending_interval_us) {
            reset_scale_sample(stream); stream->applied_at_ms=now;
        }
        stream->applied_us=c->pending_interval_us; ++c->report.accepted;
    }
    else { stream->blocked=1; ++c->report.unsupported; }
    c->next_command_ms=now+(c->attempts>1 ? MR_ACK_MS : MR_PACE_MS);
    c->pending=c->queued=c->attempts=0;
}
static int own_rate_ack(const mr_controller *c,const mr_wire *w,uint64_t now) {
    if(w->id!=MR_COMMAND_ACK || w->signed_frame || w->len<2 ||
       w->sysid!=c->sysid || w->compid!=c->compid || u16(w->payload)!=MR_SET_INTERVAL) return 0;
    if((w->len>8 && w->payload[8] && w->payload[8]!=MR_OWN_SYSTEM) ||
       (w->len>9 && w->payload[9] && w->payload[9]!=MR_OWN_COMPONENT)) return 0;
    return (w->len>9 && w->payload[9]==MR_OWN_COMPONENT) || (c->pending && now>=c->pause_until_ms);
}
void mr_observe(mr_controller *c,const uint8_t *frame,size_t length,uint64_t now) {
    mr_wire w; unsigned i; int control_reply;
    if(!c->enabled) return;
    c->window_bytes+=length;
    if(!view(frame,length,&w)) { c->window_unmanaged_bytes+=(unsigned)length; return; }
    if(w.id==MR_RADIO_STATUS && !w.signed_frame) {
        unsigned free_percent=w.len>6 ? w.payload[6] : 0;
        c->report.radio_txbuf=free_percent<=100 ? (int)free_percent : -1;
        c->radio_received_ms=now; c->have_radio=1;
    }
    if(w.sysid!=c->sysid || w.compid!=c->compid) { c->window_unmanaged_bytes+=(unsigned)length; return; }
    control_reply=own_rate_ack(c,&w,now);
    if(control_reply) c->window_control_bytes+=(unsigned)length;
    for(i=0;i<MR_STREAMS;++i) if(definitions[i].id==w.id) {
        ++c->stream[i].observed; ++c->stream[i].window_frames;
        c->stream[i].window_bytes+=(unsigned)length;
        c->observed_mask|=UINT32_C(1)<<i; break;
    }
    if(i==MR_STREAMS && w.id!=0 && !control_reply) c->window_unmanaged_bytes+=(unsigned)length;
    if(!control_reply || !c->pending || c->queued || now<c->pause_until_ms) return;
    /* MAVLink2 may trim even base-field trailing zeroes: a two-byte ACK of
     * command 511 has implicit result ACCEPTED, not a malformed result. */
    if(w.len>2 && (w.payload[2]==MR_ACK_PROGRESS || w.payload[2]==MR_ACK_TEMPORARY)) return;
    finish_command(c,now,w.len<=2 || w.payload[2]==MR_ACK_ACCEPTED);
}
static void clear_stable_window(mr_controller *c) {
    c->stable_bytes=c->stable_primary_frames=c->stable_ms=0;
}
static void cancel_probe(mr_controller *c) {
    if(c->probe_active) {
        c->report.budget_bps=minimum(c->report.budget_bps,c->probe_previous_budget);
        ++c->report.probe_rollbacks;
    }
    c->probe_active=c->probe_hold=0; clear_stable_window(c);
}
void mr_external(mr_controller *c,const uint8_t *frame,size_t length,uint64_t now) {
    mr_wire w; unsigned i;
    if(!c->enabled || !view(frame,length,&w)) return;
    if(w.id!=MR_REQUEST_STREAM && !((w.id==MR_COMMAND_LONG || w.id==MR_COMMAND_INT) && w.len>=30 && u16(w.payload+28)==MR_SET_INTERVAL)) return;
    cancel_probe(c);
    c->pending=c->queued=c->attempts=0;
    c->pause_until_ms=now+MR_GCS_PAUSE_MS;
    c->next_command_ms=c->pause_until_ms;
    for(i=0;i<MR_STREAMS;++i) { c->stream[i].applied_us=0; reset_scale_sample(&c->stream[i]); }
    c->report.state=MR_WAIT_GCS;
}
static unsigned measured_rate(uint64_t count,uint64_t elapsed,unsigned scale) {
    return elapsed ? (unsigned)(count*scale/elapsed) : 0;
}
static unsigned smoothed(unsigned previous,unsigned current,int initialized) {
    return initialized ? (unsigned)(((uint64_t)previous*(MR_MEASUREMENT_WEIGHT-1u)+current)/MR_MEASUREMENT_WEIGHT) : current;
}
/* A single slow publisher is not evidence of a shared link multiplier. Require
 * three stable streams across navigation and sensor/background roles, agreeing
 * with the largest observed ratio. The upper end is deliberately conservative:
 * one faster stream prevents a slower subset from claiming global throttling. */
static int update_common_scale(mr_controller *c,uint64_t elapsed,uint64_t now) {
    enum { NAVIGATION=1, SENSOR=2, BACKGROUND=4 };
    unsigned i,upper=0,count=0,primary=0,roles=0,proposed;
    int unsafe=c->congested || now<c->pause_until_ms ||
        c->report.unsupported+c->report.timed_out!=c->faults_seen;
    for(i=0;i<=MR_QUATERNION;++i) {
        mr_stream *s=&c->stream[i];
        if(unsafe || s->blocked || !s->applied_us) { reset_scale_sample(s); continue; }
        if(c->window_ms<s->applied_at_ms+MR_PROBE_SETTLE_MS) continue;
        s->scale_frames+=s->window_frames; s->scale_ms+=elapsed;
        if(s->scale_ms>=MR_SCALE_WINDOW_MS) {
            s->scale_milli=s->scale_frames>=MR_SCALE_MIN_STREAMS ?
                minimum(MR_MILLIHZ,(unsigned)(s->scale_frames*s->applied_us/s->scale_ms)) : 0;
            s->scale_at_ms=now; s->scale_frames=s->scale_ms=0;
        }
        if(now-s->scale_at_ms<=MR_SCALE_WINDOW_MS*2u && s->scale_milli>upper) upper=s->scale_milli;
    }
    if(!upper) return 0;
    for(i=0;i<=MR_QUATERNION;++i) {
        const mr_stream *s=&c->stream[i];
        if(now-s->scale_at_ms>MR_SCALE_WINDOW_MS*2u ||
           (uint64_t)s->scale_milli*(100u+MR_SCALE_AGREEMENT_PERCENT)<(uint64_t)upper*100u) continue;
        ++count;
        if(definitions[i].priority==MR_PRIMARY) ++primary;
        roles|=i==MR_GPS || i==MR_GLOBAL_POSITION || i==MR_ESTIMATOR ? NAVIGATION :
            definitions[i].priority==MR_PRIMARY ? SENSOR : BACKGROUND;
    }
    if(count<MR_SCALE_MIN_STREAMS || primary<2 || !(roles&NAVIGATION) || !(roles&(SENSOR|BACKGROUND))) return 0;
    proposed=(upper+MR_SCALE_QUANTUM_MILLI-1u)/MR_SCALE_QUANTUM_MILLI*MR_SCALE_QUANTUM_MILLI;
    if(proposed<MR_SCALE_MIN_MILLI) proposed=MR_SCALE_MIN_MILLI;
    {
        unsigned old=c->report.rate_scale_milli,delta=old>proposed ? old-proposed : proposed-old;
        if((uint64_t)delta*100u<=(uint64_t)old*MR_HYSTERESIS_PERCENT) return 0;
    }
    /* Keep the last stable multiplier across subsequent command changes, but
     * gather a fresh probe baseline after changing the shared cost model. */
    cancel_probe(c); c->report.rate_scale_milli=proposed;
    return 1;
}
static unsigned minimum_gain(unsigned baseline,unsigned percent) {
    unsigned gain=(unsigned)(((uint64_t)baseline*percent+99u)/100u);
    return gain ? gain : 1;
}
static int improved(unsigned current,unsigned baseline,unsigned percent) {
    return current>baseline && current-baseline>=minimum_gain(baseline,percent);
}
/* Raw unsigned/truncated packet bytes cannot be compared directly with our
 * signed/max-length planning budget to infer spare link capacity. Instead make
 * one bounded trial, wait for application/settling, and retain it only if actual
 * throughput or important-message frequency improves. Publisher-limited links
 * roll back and hold until meaningful new traffic provides fresh evidence. */
static void probe_window(mr_controller *c,unsigned primary_frames,uint64_t elapsed,uint64_t now) {
    uint64_t faults=c->report.unsupported+c->report.timed_out;
    uint64_t telemetry_bytes=c->window_bytes-c->window_control_bytes;
    unsigned bps,primary_millihz;
    if(c->congested || faults!=c->faults_seen || now<c->pause_until_ms) {
        cancel_probe(c); c->faults_seen=faults; return;
    }
    if(c->probe_active) {
        if(now-c->probe_started_ms<=MR_PROBE_SETTLE_MS) return;
        c->probe_bytes+=telemetry_bytes; c->probe_primary_frames+=primary_frames; c->probe_ms+=elapsed;
        if(c->probe_ms<MR_PROBE_MS) return;
        bps=measured_rate(c->probe_bytes,c->probe_ms,1000);
        primary_millihz=measured_rate(c->probe_primary_frames,c->probe_ms,1000000);
        c->probe_active=0;
        if(improved(bps,c->baseline_bps,MR_PROBE_GAIN_PERCENT) ||
           improved(primary_millihz,c->baseline_primary_millihz,MR_PROBE_GAIN_PERCENT)) {
            /* The verified response supplies the next baseline. No unverified
             * second increase can be layered over a pending trial. */
            c->stable_bytes=c->probe_bytes; c->stable_primary_frames=c->probe_primary_frames;
            c->stable_ms=c->probe_ms;
        } else {
            c->report.budget_bps=c->probe_previous_budget;
            ++c->report.probe_rollbacks; c->probe_hold=1;
            c->held_bps=bps; c->held_primary_millihz=primary_millihz;
            c->held_mask=c->observed_mask; clear_stable_window(c);
        }
        return;
    }
    c->stable_bytes+=telemetry_bytes; c->stable_primary_frames+=primary_frames; c->stable_ms+=elapsed;
    if(c->probe_hold) {
        if(c->held_mask!=c->observed_mask) { c->probe_hold=0; clear_stable_window(c); return; }
        if(c->stable_ms<MR_PROBE_MS) return;
        bps=measured_rate(c->stable_bytes,c->stable_ms,1000);
        primary_millihz=measured_rate(c->stable_primary_frames,c->stable_ms,1000000);
        if(improved(bps,c->held_bps,MR_REARM_GAIN_PERCENT) ||
           improved(primary_millihz,c->held_primary_millihz,MR_REARM_GAIN_PERCENT)) c->probe_hold=0;
        clear_stable_window(c); return;
    }
    if(c->stable_ms<MR_PROBE_MS || c->pending || !c->report.accepted ||
       (c->report.radio_txbuf>=0 && c->report.radio_txbuf<MR_RADIO_PROBE_PERCENT)) return;
    bps=measured_rate(c->stable_bytes,c->stable_ms,1000);
    primary_millihz=measured_rate(c->stable_primary_frames,c->stable_ms,1000000);
    {
        unsigned percent=bps>=scaled(c->report.budget_bps,MR_UTILIZATION_PERCENT) ? MR_GROW_PERCENT : MR_SLOW_GROW_PERCENT;
        unsigned increase=minimum_gain(c->report.budget_bps,percent-100u);
        unsigned proposed;
        proposed=minimum(c->uart_ceiling_bps,c->report.budget_bps+increase);
        if(proposed<=c->report.budget_bps) return;
        c->probe_previous_budget=c->report.budget_bps; c->report.budget_bps=proposed;
    }
    c->baseline_bps=bps; c->baseline_primary_millihz=primary_millihz;
    c->probe_started_ms=now; c->probe_bytes=c->probe_primary_frames=c->probe_ms=0;
    c->probe_active=1; ++c->report.probes; clear_stable_window(c);
}
void mr_tick(mr_controller *c,const mi_device *device,uint64_t reboots,uint64_t now,unsigned heartbeat_timeout) {
    unsigned observed,unmanaged,i,primary_frames;
    uint64_t elapsed,packets,gaps;
    if(!c->enabled) return;
    if(!device || !device->have_heartbeat || device->autopilot==MR_AUTOPILOT_INVALID ||
       device->type==MR_TYPE_GCS || now<device->last_heartbeat_ms || now-device->last_heartbeat_ms>=heartbeat_timeout) {
        cancel_probe(c);
        c->report.state=MR_WAIT_LINK; return;
    }
    if(c->sysid!=device->sysid || c->compid!=device->compid || c->reboots!=reboots) {
        unsigned ceiling=c->uart_ceiling_bps;
        mr_init(c,1,0); c->uart_ceiling_bps=ceiling;
        c->sysid=device->sysid; c->compid=device->compid; c->reboots=reboots;
        c->window_ms=now; c->last_packets=device->packets; c->last_gaps=device->sequence_gaps;
        c->report.state=MR_OBSERVING;
    }
    if(c->have_radio && now-c->radio_received_ms>MR_WINDOW_MS*2u) c->report.radio_txbuf=-1;
    elapsed=now-c->window_ms;
    if(elapsed>=(c->report.budget_bps ? MR_WINDOW_MS : MR_OBSERVE_MS)) {
        observed=measured_rate(c->window_bytes,elapsed,1000);
        unmanaged=measured_rate(c->window_unmanaged_bytes,elapsed,1000);
        packets=device->packets>=c->last_packets ? device->packets-c->last_packets : 0;
        gaps=device->sequence_gaps>=c->last_gaps ? device->sequence_gaps-c->last_gaps : 0;
        c->congested=(packets+gaps && gaps*100u>(packets+gaps)*MR_LOSS_PERCENT) ||
            (c->report.radio_txbuf>=0 && c->report.radio_txbuf<MR_RADIO_LOW_PERCENT);
        c->report.observed_bps=observed;
        if(!c->report.budget_bps) c->report.budget_bps=minimum(c->uart_ceiling_bps,
            scaled(measured_rate(c->window_bytes-c->window_control_bytes,elapsed,1000),MR_INITIAL_PERCENT)+command_reserve_bps());
        else if(c->congested) {
            c->report.budget_bps=scaled(c->report.budget_bps,MR_SHRINK_PERCENT);
            if(!c->report.budget_bps) c->report.budget_bps=1;
        }
        primary_frames=0;
        for(i=0;i<MR_STREAMS;++i) if(definitions[i].priority==MR_PRIMARY) primary_frames+=c->stream[i].window_frames;
        if(!update_common_scale(c,elapsed,now)) probe_window(c,primary_frames,elapsed,now);
        for(i=0;i<MR_STREAMS;++i) {
            mr_stream *stream=&c->stream[i];
            unsigned bps=measured_rate(stream->window_bytes,elapsed,1000);
            int initialized=stream->observed>stream->window_frames;
            stream->observed_millihz=measured_rate(stream->window_frames,elapsed,1000000);
            stream->average_millihz=smoothed(stream->average_millihz,stream->observed_millihz,initialized);
            stream->average_bps=smoothed(stream->average_bps,bps,initialized);
            stream->window_frames=stream->window_bytes=0;
        }
        c->average_unmanaged_bps=smoothed(c->average_unmanaged_bps,unmanaged,c->have_measurement!=0);
        c->have_measurement=1; plan(c,c->average_unmanaged_bps);
        c->window_ms=now; c->window_bytes=c->window_unmanaged_bytes=c->window_control_bytes=0;
        c->last_packets=device->packets; c->last_gaps=device->sequence_gaps;
    }
    if(!c->report.budget_bps) { c->report.state=MR_OBSERVING; return; }
    if(now<c->pause_until_ms) { c->report.state=MR_WAIT_GCS; return; }
    if(c->pending && !c->queued && now-c->sent_ms>=MR_ACK_MS) {
        if(c->attempts>=MR_MAX_ATTEMPTS) {
            c->stream[c->pending-1u].blocked=1;
            ++c->report.timed_out; c->pending=c->attempts=0;
            /* Untargeted late ACKs have no message ID. Quarantine before the
             * next stream so a previous command's reply is not misattributed. */
            c->next_command_ms=now+MR_ACK_MS;
        } else c->queued=2; /* retry may be taken by mr_next */
    }
    c->report.state=c->congested ? MR_CONGESTED : c->probe_active ? MR_PROBING :
        c->probe_hold ? MR_HOLD : c->pending ? MR_APPLYING : MR_MONITORING;
}
/* Serialize float bits through memcpy, avoiding the upstream convenience
 * macro's type-punned store under the project's strict-aliasing build. */
static void put_float(char *destination,float value) {
    uint32_t bits; unsigned i;
    memcpy(&bits,&value,sizeof(bits));
    for(i=0;i<sizeof(bits);++i) destination[i]=(char)(bits>>(i*CHAR_BIT));
}
size_t mr_next(mr_controller *c,uint8_t sequence,uint8_t *out,size_t capacity,uint64_t now) {
    mavlink_message_t message; mavlink_status_t status; unsigned i,phase;
    const mavlink_msg_entry_t *entry=mavlink_get_msg_entry(MR_COMMAND_LONG);
    char *payload; size_t command_offset=MR_COMMAND_PARAMETERS*MR_PARAMETER_BYTES;
    if(!entry || !out || capacity<(size_t)entry->max_msg_len+MAVLINK_NUM_NON_PAYLOAD_BYTES) return 0;
    if(!c->enabled || c->report.state==MR_WAIT_LINK || c->report.state==MR_OBSERVING ||
       c->report.state==MR_WAIT_GCS || now<c->next_command_ms || c->queued==1) return 0;
    if(c->pending && c->queued!=2) return 0;
    if(!c->pending) {
        /* First reduce known secondary streams, then raise primary rates.
         * This releases existing bandwidth before enabling more telemetry. */
        for(phase=0;phase<2 && !c->pending;++phase) for(i=0;i<MR_STREAMS;++i) {
            unsigned index=(c->scan+i)%MR_STREAMS;
            mr_stream *stream=&c->stream[index];
            if((definitions[index].priority!=MR_PRIMARY)!=(phase==0)) continue;
            if(stream->blocked || !stream->desired_us || stream->applied_us==stream->desired_us) continue;
            c->pending=index+1u; c->scan=(index+1u)%MR_STREAMS;
            c->pending_interval_us=stream->desired_us; c->attempts=0; break;
        }
        if(!c->pending) return 0;
    }
    memset(&message,0,sizeof(message)); memset(&status,0,sizeof(status));
    message.msgid=MR_COMMAND_LONG; status.current_tx_seq=sequence;
    payload=_MAV_PAYLOAD_NON_CONST(&message);
    put_float(payload,(float)definitions[c->pending-1u].id);
    put_float(payload+MR_PARAMETER_BYTES,(float)c->pending_interval_us);
    payload[command_offset]=(char)(MR_SET_INTERVAL&UCHAR_MAX);
    payload[command_offset+1u]=(char)(MR_SET_INTERVAL>>CHAR_BIT);
    payload[command_offset+2u]=(char)c->sysid;
    payload[command_offset+3u]=(char)c->compid;
    payload[command_offset+4u]=(char)c->attempts;
    (void)mavlink_finalize_message_buffer(&message,MR_OWN_SYSTEM,MR_OWN_COMPONENT,&status,
        entry->min_msg_len,entry->max_msg_len,entry->crc_extra);
    c->queued=1; ++c->attempts; ++c->report.commands; c->report.state=MR_APPLYING;
    return mavlink_msg_to_send_buffer(out,&message);
}
void mr_sent(mr_controller *c,uint64_t now) {
    if(c->pending && c->queued==1) { c->queued=0; c->sent_ms=now; }
}
