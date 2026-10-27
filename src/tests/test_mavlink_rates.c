/* Deterministic clocks and independent wire checks; no device or network I/O. */
#include "mavlink_rates.h"
#include <stdio.h>
#include <string.h>

enum { FRAME_CAP = 280, ACK_ID = 77, ACK_CRC = 143, COMMAND_CRC = 152,
       COMMAND_ID = 76, SET_INTERVAL = 511 };
static unsigned checks;
#define CHECK(test) do { ++checks; if(!(test)) { fprintf(stderr,"FAIL %s:%d %s\n",__FILE__,__LINE__,#test); return 0; } } while(0)
static uint16_t crc_byte(uint16_t crc,uint8_t byte) {
    uint8_t value=(uint8_t)(byte^crc); value^=(uint8_t)(value<<4);
    return (uint16_t)((crc>>8)^((uint16_t)value<<8)^((uint16_t)value<<3)^(value>>4));
}
static unsigned read_u16(const uint8_t *p) { return (unsigned)p[0]|(unsigned)p[1]<<8; }
static uint32_t read_u32(const uint8_t *p) { return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static float read_float(const uint8_t *p) { uint32_t bits=read_u32(p); float result; memcpy(&result,&bits,sizeof(result)); return result; }
static size_t packet(uint8_t *out,uint32_t id,uint8_t system,uint8_t component,
                     const uint8_t *payload,size_t size,uint8_t extra) {
    size_t i,n=0; uint16_t crc=UINT16_MAX;
    out[n++]=0xfd; out[n++]=(uint8_t)size; out[n++]=0; out[n++]=0;
    out[n++]=22; out[n++]=system; out[n++]=component;
    out[n++]=(uint8_t)id; out[n++]=(uint8_t)(id>>8); out[n++]=(uint8_t)(id>>16);
    memcpy(out+n,payload,size); n+=size;
    for(i=1;i<n;++i) crc=crc_byte(crc,out[i]);
    crc=crc_byte(crc,extra); out[n++]=(uint8_t)crc; out[n++]=(uint8_t)(crc>>8);
    return n;
}
static void observe(mr_controller *c,uint32_t id,unsigned payload_size,unsigned count,uint64_t now) {
    uint8_t frame[FRAME_CAP],payload[255]={0}; unsigned i;
    size_t n=packet(frame,id,1,1,payload,payload_size,0);
    /* mr_observe's caller has already validated framing/CRC. These are byte
     * accounting fixtures; ACK/command protocol fixtures below have real CRCs. */
    for(i=0;i<count;++i) mr_observe(c,frame,n,now);
}
static void acknowledge(mr_controller *c,unsigned result,unsigned system,unsigned destination,uint64_t now) {
    uint8_t frame[FRAME_CAP],payload[10]={255,1,0,0,0,0,0,0,255,191};
    size_t n; payload[2]=(uint8_t)result; payload[9]=(uint8_t)destination;
    n=packet(frame,ACK_ID,(uint8_t)system,1,payload,sizeof(payload),ACK_CRC);
    mr_observe(c,frame,n,now);
}
static mi_device vehicle(uint64_t now) {
    mi_device d; memset(&d,0,sizeof(d)); d.sysid=1; d.compid=1;
    d.have_heartbeat=1; d.last_heartbeat_ms=now; return d;
}
static void tick(mr_controller *c,mi_device *d,uint64_t now) {
    d->last_heartbeat_ms=now; d->packets+=10; mr_tick(c,d,0,now,5000);
}
static void warmup(mr_controller *c,mi_device *d) {
    mr_init(c,1,57600); tick(c,d,1000);
    observe(c,30,28,15,2000); observe(c,31,32,10,2000);
    observe(c,24,30,3,2000); observe(c,33,28,4,2000);
    observe(c,12920,5,1,2000); observe(c,74,20,3,2000);
    observe(c,230,42,1,2000); observe(c,1,31,1,2000);
    tick(c,d,4000);
}
static int valid_command(const uint8_t *frame,size_t length,unsigned sequence,unsigned *id,unsigned *interval) {
    size_t i; uint16_t crc=UINT16_MAX;
    CHECK(length>=44 && length<=45); CHECK(frame[0]==0xfd && frame[2]==0 && frame[3]==0);
    CHECK(frame[4]==sequence && frame[5]==255 && frame[6]==191);
    CHECK(frame[7]==COMMAND_ID && frame[8]==0 && frame[9]==0);
    CHECK(length==(size_t)frame[1]+12u);
    for(i=1;i<length-2u;++i) crc=crc_byte(crc,frame[i]);
    CHECK(crc_byte(crc,COMMAND_CRC)==read_u16(frame+length-2u));
    CHECK(read_u16(frame+38)==SET_INTERVAL && frame[40]==1 && frame[41]==1);
    for(i=18;i<38;++i) CHECK(frame[i]==0);
    *id=(unsigned)read_float(frame+10); *interval=(unsigned)read_float(frame+14);
    CHECK(*id!=0 && *interval>0); return 1;
}
static int protocol_and_priorities(void) {
    mr_controller c; mi_device d=vehicle(1000); uint8_t frame[FRAME_CAP];
    unsigned id,interval,seen=0,sequence=0; uint64_t now=4000; size_t n;
    warmup(&c,&d);
    CHECK(c.report.observed_bps>0 && c.report.budget_bps<1000);
    CHECK(c.report.budget_bps<=57600u/10u*70u/100u);
    n=mr_next(&c,(uint8_t)sequence,frame,sizeof(frame),now);
    CHECK(valid_command(frame,n,sequence++,&id,&interval));
    CHECK(id==30 || id==31); /* Release attitude bandwidth first. */
    CHECK(interval>=500000u);
    CHECK(mr_next(&c,0,frame,sizeof(frame),now+10000)==0); /* Partial write owns frame. */
    mr_sent(&c,now);
    acknowledge(&c,0,2,191,now+50); CHECK(c.report.accepted==0);
    acknowledge(&c,0,1,190,now+50); CHECK(c.report.accepted==0);
    acknowledge(&c,0,1,191,now+100); CHECK(c.report.accepted==1);
    while(now<8000) {
        now+=600; tick(&c,&d,now);
        n=mr_next(&c,(uint8_t)sequence,frame,sizeof(frame),now);
        if(!n) continue;
        CHECK(valid_command(frame,n,sequence++,&id,&interval));
        CHECK(id!=1); /* Existing 0.3Hz status must not be raised to 1Hz. */
        CHECK(id!=8009 && id!=295); /* No duplicate weather/airspeed activation. */
        if(id==24) seen|=1u;
        if(id==33) seen|=2u;
        if(id==12920) seen|=4u;
        mr_sent(&c,now); acknowledge(&c,0,1,191,now+20);
    }
    CHECK(seen==7u); CHECK(c.report.accepted>=6);
    return 1;
}
static int retries_and_conflict(void) {
    mr_controller c; mi_device d=vehicle(1000); uint8_t frame[FRAME_CAP],external[FRAME_CAP];
    size_t n; unsigned id,interval,first;
    warmup(&c,&d); n=mr_next(&c,0,frame,sizeof(frame),4000);
    CHECK(valid_command(frame,n,0,&id,&interval)); first=id;
    mr_sent(&c,4000); tick(&c,&d,5999);
    CHECK(mr_next(&c,1,frame,sizeof(frame),5999)==0);
    tick(&c,&d,6000); n=mr_next(&c,1,frame,sizeof(frame),6000);
    CHECK(valid_command(frame,n,1,&id,&interval)); CHECK(id==first && frame[42]==1);
    mr_sent(&c,6000); tick(&c,&d,8000);
    CHECK(c.report.timed_out==1 && c.report.commands==2);
    CHECK(mr_next(&c,2,frame,sizeof(frame),9999)==0);
    tick(&c,&d,10000); n=mr_next(&c,2,frame,sizeof(frame),10000);
    CHECK(valid_command(frame,n,2,&id,&interval)); CHECK(id!=first);
    memcpy(external,frame,n); external[6]=190;
    mr_sent(&c,10000); mr_external(&c,external,n,10001);
    acknowledge(&c,0,1,0,10002); CHECK(c.report.accepted==0);
    tick(&c,&d,12000); CHECK(c.report.state==MR_WAIT_GCS);
    CHECK(mr_next(&c,3,frame,sizeof(frame),12000)==0);
    tick(&c,&d,15002); n=mr_next(&c,3,frame,sizeof(frame),15002);
    CHECK(valid_command(frame,n,3,&id,&interval));
    mr_sent(&c,15002); acknowledge(&c,3,1,191,15003);
    CHECK(c.report.unsupported==1);
    return 1;
}
static int adaptive_and_safety(void) {
    mr_controller c; mi_device d=vehicle(1000); uint8_t frame[FRAME_CAP],radio[9]={0};
    unsigned budget; size_t n;
    warmup(&c,&d); budget=c.report.budget_bps;
    radio[6]=5; n=packet(frame,109,51,68,radio,sizeof(radio),185);
    mr_observe(&c,frame,n,5000); observe(&c,30,28,100,5000); tick(&c,&d,9000);
    CHECK(c.report.state==MR_CONGESTED && c.report.budget_bps<budget);
    d.last_heartbeat_ms=0; mr_tick(&c,&d,0,15000,5000);
    CHECK(c.report.state==MR_WAIT_LINK && mr_next(&c,0,frame,sizeof(frame),15000)==0);
    d.last_heartbeat_ms=15000; mr_tick(&c,&d,1,15000,5000);
    CHECK(c.report.state==MR_OBSERVING && c.report.commands==0 && c.report.accepted==0);
    warmup(&c,&d); c.report.budget_bps=1;
    observe(&c,22,25,100,5000); tick(&c,&d,9000);
    CHECK(c.report.budget_bps==1); /* Growth waits for a stable, measured trial. */
    CHECK(c.report.state==MR_CONGESTED);
    n=mr_next(&c,0,frame,sizeof(frame),9000);
    CHECK(n>0 && read_float(frame+14)==5000000.0f); /* Recover actively; never leave the fast plan running. */
    d.autopilot=8; tick(&c,&d,9001);
    CHECK(c.report.state==MR_WAIT_LINK && mr_next(&c,0,frame,sizeof(frame),9001)==0);
    d.autopilot=0; d.type=6; tick(&c,&d,9002);
    CHECK(c.report.state==MR_WAIT_LINK);
    mr_init(&c,0,57600); mr_tick(&c,&d,0,20000,5000);
    CHECK(c.report.state==MR_DISABLED && mr_next(&c,0,frame,sizeof(frame),20000)==0);
    return 1;
}
static int trimmed_ack_and_signed_reply(void) {
    mr_controller c; mi_device d=vehicle(1000);
    uint8_t frame[FRAME_CAP],payload[2]={255,1}; size_t n;
    warmup(&c,&d); CHECK(mr_next(&c,0,frame,sizeof(frame),4000)>0); mr_sent(&c,4000);
    n=packet(frame,ACK_ID,1,1,payload,sizeof(payload),ACK_CRC);
    frame[2]=1; /* Caller validated the signed wire; this host cannot authenticate it. */
    mr_observe(&c,frame,n,4050); CHECK(c.report.accepted==0);
    frame[2]=0; mr_observe(&c,frame,n,4100); CHECK(c.report.accepted==1);
    return 1;
}
static void probe_fixture(mr_controller *c,mi_device *d) {
    warmup(c,d); c->report.budget_bps=1000;
    c->report.accepted=1; /* The preceding protocol tests verify real ACK attribution. */
    c->stable_bytes=c->stable_primary_frames=c->stable_ms=0;
}
static void probe_sample(mr_controller *c,mi_device *d,uint64_t *now,unsigned samples,int radio_free) {
    uint8_t frame[FRAME_CAP],payload[9]={0}; size_t n;
    *now+=MR_WINDOW_MS;
    observe(c,12920,5,samples,*now); /* A real HYG frame is only 17 wire bytes. */
    if(radio_free>=0) {
        payload[6]=(uint8_t)radio_free;
        n=packet(frame,109,51,68,payload,sizeof(payload),185); mr_observe(c,frame,n,*now);
    }
    tick(c,d,*now);
}
static void own_ack_noise(mr_controller *c,unsigned count,uint64_t now) {
    unsigned i;
    for(i=0;i<count;++i) acknowledge(c,0,1,191,now);
}
static int bounded_probe_no_response(void) {
    mr_controller c; mi_device d=vehicle(1000); uint64_t now=4000; unsigned i,changed=0;
    uint32_t previous[MR_STREAM_CAP];
    probe_fixture(&c,&d);
    for(i=0;i<3;++i) probe_sample(&c,&d,&now,20,-1);
    for(i=0;i<MR_STREAM_CAP;++i) previous[i]=c.stream[i].desired_us;
    probe_sample(&c,&d,&now,20,-1);
    for(i=0;i<MR_STREAM_CAP;++i) if(previous[i]!=c.stream[i].desired_us) ++changed;
    CHECK(c.report.observed_bps*100u<c.report.budget_bps*65u);
    CHECK(c.probe_active && c.report.probes==1 && c.report.budget_bps==1050);
    CHECK(changed>0); /* A 5% bandwidth trial must bypass the 10% noise hysteresis. */
    for(i=0;i<5;++i) {
        own_ack_noise(&c,100,now+10); /* Our control replies must not masquerade as improved telemetry. */
        probe_sample(&c,&d,&now,20,-1);
    }
    CHECK(!c.probe_active && c.probe_hold && c.report.budget_bps==1000);
    CHECK(c.report.probe_rollbacks==1);
    for(i=0;i<16;++i) probe_sample(&c,&d,&now,20,-1);
    CHECK(c.report.probes==1 && c.report.state==MR_HOLD); /* No unbounded climb on a slow publisher. */
    observe(&c,295,12,1,now); probe_sample(&c,&d,&now,20,-1);
    CHECK(!c.probe_hold); /* A newly appearing sensor permits a fresh measured trial. */
    return 1;
}
static int probe_response_and_congestion(void) {
    mr_controller c; mi_device d=vehicle(1000); uint64_t now=4000; unsigned i,budget;
    probe_fixture(&c,&d);
    for(i=0;i<4;++i) probe_sample(&c,&d,&now,20,-1);
    for(i=0;i<5;++i) probe_sample(&c,&d,&now,30,-1);
    CHECK(!c.probe_active && !c.probe_hold && c.report.budget_bps==1050);
    probe_sample(&c,&d,&now,30,-1); CHECK(c.probe_active && c.report.probes==2);
    budget=c.probe_previous_budget; d.sequence_gaps+=100;
    probe_sample(&c,&d,&now,30,-1);
    CHECK(!c.probe_active && c.report.budget_bps<budget && c.report.state==MR_CONGESTED);
    probe_fixture(&c,&d); now=4000;
    for(i=0;i<5;++i) probe_sample(&c,&d,&now,20,40);
    CHECK(c.report.probes==0); /* Known radio with little spare buffer cannot be probed. */
    probe_sample(&c,&d,&now,20,80); CHECK(c.probe_active);
    budget=c.probe_previous_budget; probe_sample(&c,&d,&now,20,5);
    CHECK(!c.probe_active && c.report.budget_bps<budget && c.report.state==MR_CONGESTED);
    probe_fixture(&c,&d); now=4000; c.report.budget_bps=1;
    for(i=0;i<4;++i) probe_sample(&c,&d,&now,20,-1);
    CHECK(c.probe_active && c.report.budget_bps>1); /* Ceiling growth recovers from tiny budgets. */
    return 1;
}
static int periodic_allowlist(void) {
    mr_controller c; mi_device d=vehicle(1000); uint8_t frame[FRAME_CAP];
    unsigned id,interval,seen=0,i; uint64_t now=4000; size_t n;
    warmup(&c,&d); observe(&c,83,37,20,5000); observe(&c,141,32,20,5000);
    observe(&c,82,39,20,5000); /* SET_ATTITUDE_TARGET is a command: never govern it. */
    tick(&c,&d,9000); now=9000;
    for(i=0;i<12;++i) {
        n=mr_next(&c,(uint8_t)i,frame,sizeof(frame),now);
        if(!n) break;
        CHECK(valid_command(frame,n,i,&id,&interval)); CHECK(id!=82);
        if(id==83) { CHECK(interval>=1000000u); seen|=1u; }
        if(id==141) { CHECK(interval>=1000000u); seen|=2u; }
        mr_sent(&c,now); acknowledge(&c,0,1,191,now+10); now+=600;
    }
    CHECK(seen==3u); return 1;
}
static void measured_traffic(mr_controller *c,uint64_t now,unsigned hyg_samples) {
    observe(c,30,28,25,now); observe(c,31,32,17,now);
    observe(c,24,30,5,now); observe(c,33,28,7,now);
    observe(c,12920,5,hyg_samples,now); observe(c,74,20,5,now);
    observe(c,230,42,2,now); observe(c,1,31,2,now);
}
static int essential_floor_before_background(void) {
    mr_controller c; mi_device d=vehicle(1000); uint8_t frame[FRAME_CAP];
    unsigned i,id,interval,primary=0,slower_background=0; size_t n; uint64_t now=9000;
    warmup(&c,&d); c.report.budget_bps=440;
    measured_traffic(&c,5000,2); observe(&c,147,1,4,5000);
    tick(&c,&d,now);
    for(i=0;i<10;++i) {
        n=mr_next(&c,(uint8_t)i,frame,sizeof(frame),now); if(!n) break;
        CHECK(valid_command(frame,n,i,&id,&interval));
        if(id==24 || id==33 || id==12920 || id==74 || id==230) {
            CHECK(interval<=1000000u); ++primary;
        } else if(interval>1000000u) ++slower_background;
        CHECK(id!=147); /* Already below 1Hz: preserve it and reserve its actual short frame. */
        mr_sent(&c,now); acknowledge(&c,0,1,191,now+10); now+=600;
    }
    CHECK(primary==5 && slower_background>0);
    CHECK(!c.congested); /* Sufficient for essentials, although not every background 1Hz baseline. */
    return 1;
}
static unsigned drain_window(mr_controller *c,uint64_t now) {
    uint8_t frame[FRAME_CAP]; unsigned i,sent=0;
    for(i=0;i<9;++i) {
        uint64_t at=now+i*550u;
        size_t n=mr_next(c,(uint8_t)i,frame,sizeof(frame),at);
        if(!n) continue;
        mr_sent(c,at); acknowledge(c,0,1,191,at+1); ++sent;
    }
    return sent;
}
static int stable_measurements_do_not_chatter(void) {
    mr_controller quiet,noisy; mi_device a=vehicle(1000),b=vehicle(1000);
    uint8_t radio[FRAME_CAP],payload[9]={0}; uint64_t now=4000;
    unsigned window,i,late_commands=0; size_t n;
    warmup(&quiet,&a); warmup(&noisy,&b);
    quiet.report.budget_bps=noisy.report.budget_bps=900;
    payload[6]=40; n=packet(radio,109,51,68,payload,sizeof(payload),185);
    for(window=0;window<20;++window) {
        unsigned left,right,hyg=window%2u ? 6u : 4u;
        now+=MR_WINDOW_MS;
        measured_traffic(&quiet,now,hyg); measured_traffic(&noisy,now,hyg);
        mr_observe(&quiet,radio,n,now); mr_observe(&noisy,radio,n,now);
        own_ack_noise(&noisy,100,now);
        tick(&quiet,&a,now); tick(&noisy,&b,now);
        for(i=0;i<MR_STREAM_CAP;++i) CHECK(quiet.stream[i].desired_us==noisy.stream[i].desired_us);
        CHECK(quiet.average_unmanaged_bps==noisy.average_unmanaged_bps);
        left=drain_window(&quiet,now); right=drain_window(&noisy,now); CHECK(left==right);
        if(window>=8) late_commands+=left;
    }
    CHECK(quiet.report.probes==0 && noisy.report.probes==0);
    CHECK(late_commands==0); /* ±1 samples in each five-second window cannot drive endless requests. */
    return 1;
}
typedef struct { unsigned index,id,payload,cap; } scale_fixture_stream;
static const scale_fixture_stream scale_streams[]={
    {0,24,30,10},{1,33,28,20},{2,12920,5,10},{4,74,20,10},{6,230,42,5},
    {7,30,28,2},{8,31,32,2},{29,83,37,1}
};
#define SCALE_STREAMS (sizeof(scale_streams)/sizeof(scale_streams[0]))
static void scale_fixture(mr_controller *c,mi_device *d) {
    unsigned i; warmup(c,d); c->report.budget_bps=900; c->report.accepted=1;
    for(i=0;i<SCALE_STREAMS;++i) {
        mr_stream *s=&c->stream[scale_streams[i].index];
        /* Background may be observed below its cap and never configured by
         * us. GPS/position plus VFR still establish independent shared roles. */
        if(i<5) { s->applied_us=1000000; s->applied_at_ms=4000; }
    }
    c->stream[29].applied_us=1000000; /* Previously capped background traffic. */
}
static void scale_sample(mr_controller *c,mi_device *d,uint64_t *now,int single_sensor) {
    unsigned i; uint8_t frame[FRAME_CAP],radio[9]={0}; size_t n;
    *now+=MR_WINDOW_MS*2u;
    for(i=0;i<SCALE_STREAMS;++i) {
        const scale_fixture_stream *s=&scale_streams[i];
        unsigned samples=single_sensor ? (s->id==12920 ? 1u : 10u) : 5u;
        observe(c,s->id,s->payload,samples,*now);
    }
    radio[6]=40; n=packet(frame,109,51,68,radio,sizeof(radio),185);
    mr_observe(c,frame,n,*now); /* Spare buffer allows calibration, not a new bandwidth probe. */
    tick(c,d,*now);
}
static int common_throttle_preserves_effective_priority(void) {
    mr_controller c; mi_device d=vehicle(1000); uint64_t now=4000;
    unsigned i,budget; uint32_t previous[5]; uint8_t frame[FRAME_CAP];
    scale_fixture(&c,&d);
    scale_sample(&c,&d,&now,0); scale_sample(&c,&d,&now,0);
    CHECK(c.report.rate_scale_milli==1000); /* No estimate from command settling or a short sample. */
    scale_sample(&c,&d,&now,0); CHECK(c.report.rate_scale_milli==500);
    for(i=0;i<SCALE_STREAMS;++i) {
        const scale_fixture_stream *s=&scale_streams[i];
        CHECK(c.stream[s->index].desired_us>=1000000u/s->cap);
        if(i<5) {
            CHECK(c.stream[s->index].desired_us<=500000u); /* Requested >=2Hz reserves effective >=1Hz. */
            previous[i]=c.stream[s->index].desired_us;
        }
    }
    CHECK(c.stream[7].applied_us==0 && c.stream[8].applied_us==0);
    CHECK(c.stream[29].desired_us>=1000000u);
    CHECK(c.report.probes==0); /* The estimate did not depend on a bandwidth trial. */
    CHECK(mr_next(&c,0,frame,sizeof(frame),now)>0);
    mr_sent(&c,now); acknowledge(&c,0,1,191,now+10);
    CHECK(c.report.rate_scale_milli==500); /* One changed command cannot reset the whole cost model. */
    budget=c.report.budget_bps; d.sequence_gaps+=100;
    scale_sample(&c,&d,&now,0);
    CHECK(c.report.state==MR_CONGESTED && c.report.budget_bps<budget);
    CHECK(c.report.rate_scale_milli==500); /* Loss must not be mistaken for a smaller common multiplier. */
    for(i=0;i<5;++i) CHECK(c.stream[scale_streams[i].index].desired_us>=previous[i]);
    return 1;
}
static int one_slow_sensor_is_not_common_throttling(void) {
    mr_controller c; mi_device d=vehicle(1000); uint64_t now=4000; unsigned i;
    scale_fixture(&c,&d);
    for(i=0;i<8;++i) scale_sample(&c,&d,&now,1);
    CHECK(c.report.rate_scale_milli==1000);
    scale_fixture(&c,&d); now=4000;
    for(i=0;i<4;++i) {
        unsigned j;
        for(j=0;j<5;++j) c.stream[scale_streams[j].index].applied_at_ms=now;
        scale_sample(&c,&d,&now,0);
    }
    CHECK(c.report.rate_scale_milli==1000); /* Stale traffic after recently changed commands is excluded. */
    return 1;
}
static int shared_scale_has_bounded_compensation(void) {
    mr_controller c; mi_device d=vehicle(1000); uint64_t now=4000; unsigned i;
    scale_fixture(&c,&d);
    for(i=0;i<5;++i) c.stream[scale_streams[i].index].applied_us=200000;
    for(i=0;i<3;++i) scale_sample(&c,&d,&now,0); /* Actual/requested = .5Hz/5Hz = .1. */
    CHECK(c.report.rate_scale_milli==250); /* Never extrapolate more than fourfold. */
    for(i=0;i<SCALE_STREAMS;++i) {
        const scale_fixture_stream *s=&scale_streams[i];
        CHECK(c.stream[s->index].desired_us>=1000000u/s->cap);
    }
    return 1;
}
int main(void) {
    if(!protocol_and_priorities() || !retries_and_conflict() || !adaptive_and_safety() ||
       !trimmed_ack_and_signed_reply() || !bounded_probe_no_response() ||
       !probe_response_and_congestion() || !periodic_allowlist() ||
       !essential_floor_before_background() || !stable_measurements_do_not_chatter() ||
       !common_throttle_preserves_effective_priority() || !one_slow_sensor_is_not_common_throttling() ||
       !shared_scale_has_bounded_compensation()) return 1;
    printf("mavlink_rates: %u checks passed\n",checks); return 0;
}
