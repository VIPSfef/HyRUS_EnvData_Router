/* Wire fixtures use a slow bit-at-a-time CRC implementation independent of
 * the decoder's byte-folding implementation and the official CRC_EXTRA values.
 * No serial hardware, server, or wall-clock sleeps are needed. */
#include "mavlink_input.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

static mi_input input;
static mi_event samples[4096];
static weather_sample weather_samples[128];
static unsigned weather_count;
static uint64_t sample_count, clock_count, reset_count, watermark_count, origin_count;
static uint64_t previous_time, final_watermark;
static double observed_origin[3];
static unsigned failures, checks;
static uint8_t seq;
#define CHECK(c) do { ++checks; if (!(c)) { fprintf(stderr,"FAIL %s:%d %s\n",__FILE__,__LINE__,#c); ++failures; return 0; } } while(0)

static void put16(uint8_t *p, uint16_t u) { p[0]=(uint8_t)u; p[1]=(uint8_t)(u>>8); }
static void put32(uint8_t *p, uint32_t u) { unsigned j; for(j=0;j<4;++j) p[j]=(uint8_t)(u>>(j*8)); }
static void put64(uint8_t *p, uint64_t u) { unsigned j; for(j=0;j<8;++j) p[j]=(uint8_t)(u>>(j*8)); }
static void putfloat(uint8_t *p, float f) { uint32_t u; memcpy(&u,&f,4); put32(p,u); }
static uint16_t reference_byte(uint16_t crc, uint8_t byte)
{
    unsigned j;
    crc^=byte;
    for(j=0;j<8;++j) crc=(uint16_t)((crc>>1)^((crc&1u)?0x8408u:0u));
    return crc;
}
static size_t make_frame(uint8_t *out, uint32_t id, const uint8_t *p, size_t n,
                         uint8_t extra, int version, uint8_t sysid,
                         uint8_t compid, uint8_t sequence, uint8_t flags)
{
    size_t j, h=version==2?10:6;
    uint16_t crc=UINT16_MAX;
    memset(out,0,280); out[0]=version==2?0xfd:0xfe; out[1]=(uint8_t)n;
    if(version==2) { out[2]=flags; out[4]=sequence; out[5]=sysid; out[6]=compid;
        out[7]=(uint8_t)id; out[8]=(uint8_t)(id>>8); out[9]=(uint8_t)(id>>16); }
    else { out[2]=sequence; out[3]=sysid; out[4]=compid; out[5]=(uint8_t)id; }
    memcpy(out+h,p,n);
    for(j=1;j<h+n;++j) crc=reference_byte(crc,out[j]);
    crc=reference_byte(crc,extra); put16(out+h+n,crc);
    return h+n+2+(version==2 && (flags&1)?13:0);
}
static void on_event(void *context, const mi_event *e)
{
    (void)context;
    if(e->kind==MI_SAMPLE) {
        if(sample_count && e->sample.t_us<previous_time) ++failures;
        previous_time=e->sample.t_us;
        if(sample_count<sizeof(samples)/sizeof(samples[0])) samples[sample_count]=*e;
        ++sample_count;
    } else if(e->kind==MI_WEATHER_SAMPLE) {
        if(weather_count<sizeof(weather_samples)/sizeof(weather_samples[0])) weather_samples[weather_count]=e->weather;
        ++weather_count;
    } else if(e->kind==MI_CLOCK) ++clock_count;
    else if(e->kind==MI_RESET) { ++reset_count; previous_time=0; }
    else if(e->kind==MI_WATERMARK) { ++watermark_count; final_watermark=e->boot_us; }
    else if(e->kind==MI_ORIGIN) { ++origin_count; memcpy(observed_origin,e->origin,sizeof(observed_origin)); }
}
static void initialize(void)
{
    mi_options o;
    mi_options_default(&o); o.origin_lat=37; o.origin_lon=127; o.origin_alt=100;
    mi_init(&input,&o,on_event,NULL);
    seq=0; sample_count=clock_count=reset_count=watermark_count=0;
    previous_time=final_watermark=origin_count=0; weather_count=0;
    memset(observed_origin,0,sizeof(observed_origin));
}
static void feed(uint32_t id, const uint8_t *p, size_t n, uint8_t extra,
                 uint8_t sysid, uint8_t compid, uint64_t now)
{
    uint8_t frame[280]; size_t count=make_frame(frame,id,p,n,extra,2,sysid,compid,seq++,0);
    mi_feed(&input,frame,count,now);
}
static void heartbeat(uint8_t sysid, uint8_t compid, uint8_t type, uint8_t autopilot)
{
    uint8_t p[9]={0}; p[4]=type; p[5]=autopilot; p[6]=128; p[7]=4; p[8]=3;
    feed(0,p,sizeof(p),50,sysid,compid,100);
}
static void navigation_at(uint32_t t, int32_t lat_delta, uint8_t sysid, uint64_t now)
{
    uint8_t p[28]={0};
    put32(p,t); put32(p+4,(uint32_t)(370000000+lat_delta)); put32(p+8,1270000000);
    put32(p+12,100000); put16(p+20,1000); put16(p+22,2000); put16(p+24,(uint16_t)-300);
    feed(33,p,sizeof(p),104,sysid,1,now);
}
static void navigation(uint32_t t, int32_t lat_delta, uint8_t sysid)
{ navigation_at(t,lat_delta,sysid,t); }
static void named(uint32_t t, const char *key, float value, uint8_t compid)
{
    uint8_t p[18]={0}; size_t n=strlen(key); if(n>10)n=10;
    put32(p,t); putfloat(p+4,value); memcpy(p+8,key,n);
    feed(251,p,sizeof(p),170,1,compid,t);
}
static void estimator(uint32_t t, uint16_t flags, uint64_t now)
{
    uint8_t p[42]={0}; put64(p,(uint64_t)t*1000); put16(p+40,flags);
    feed(230,p,sizeof(p),163,1,1,now);
}
static void gps(uint8_t fix, uint64_t now)
{
    uint8_t p[52]={0}; put64(p,input.high_boot_us); p[28]=fix;
    put32(p+34,2000); put32(p+38,3000); feed(24,p,sizeof(p),24,1,1,now);
}
static void local_velocity(uint32_t t, float north, float east, float down, uint64_t now)
{
    uint8_t p[28]={0}; put32(p,t);
    putfloat(p+4,1000000); putfloat(p+8,-1000000); putfloat(p+12,5000); /* deliberately unrelated local origin */
    putfloat(p+16,north); putfloat(p+20,east); putfloat(p+24,down);
    feed(32,p,sizeof(p),185,1,1,now);
}
static void atmospheric(uint64_t stamp, uint64_t now)
{
    uint8_t p[16]={0}; put64(p,stamp); putfloat(p+8,20); putfloat(p+12,50);
    feed(8009,p,sizeof(p),144,1,1,now);
}
static void trusted_anchor(uint32_t t, float north, float east, float down)
{
    initialize(); heartbeat(1,1,2,12);
    estimator(t,54,t); local_velocity(t,north,east,down,t); gps(3,t); navigation(t,0,1);
}
static int check_discovery_and_fragmentation(void)
{
    uint8_t p[9]={0}, frame[280]; size_t n,j;
    initialize();
    heartbeat(250,191,6,8); CHECK(input.report.selected_system==0);
    heartbeat(1,1,2,12); CHECK(input.report.selected_system==1 && input.report.selected_component==1);
    heartbeat(1,100,18,8); heartbeat(2,1,2,3);
    CHECK(input.report.devices==4);
    CHECK(input.report.device[1].have_heartbeat && input.report.device[1].type==2 && input.report.device[1].autopilot==12);
    CHECK(input.report.device[1].base_mode==128);
    CHECK(input.report.selected_system==1);
    p[4]=2; p[5]=12; p[8]=3;
    n=make_frame(frame,0,p,9,50,1,5,1,20,0);
    for(j=0;j<n;++j) mi_feed(&input,frame+j,1,200);
    CHECK(input.report.devices==5 && input.report.device[4].sysid==5);
    n=mi_heartbeat(frame,sizeof(frame),255,191,99); CHECK(n==17);
    mi_feed(&input,frame,n,201); CHECK(input.report.devices==6);
    CHECK(input.report.device[5].type==18 && input.report.device[5].autopilot==8);
    CHECK(mi_heartbeat(frame,16,255,191,99)==0 && mi_heartbeat(frame,sizeof(frame),0,191,99)==0);
    return 1;
}
static int check_parser_recovery(void)
{
    uint8_t p[255]={0}, frame[280]; size_t n;
    initialize();
    p[4]=2; p[5]=12; p[8]=3;
    n=make_frame(frame,0,p,9,50,2,1,1,1,0); frame[12]^=3;
    mi_feed(&input,frame,n,0); CHECK(input.report.bad_crc==1 && input.report.frames==0);
    heartbeat(1,1,2,12); CHECK(input.report.frames==1);
    n=make_frame(frame,65000,p,255,0,2,1,1,3,0);
    mi_feed(&input,frame,10,0); CHECK(input.report.unknown_message==1 && input.used<10);
    heartbeat(1,1,2,12); CHECK(input.report.frames==2);
    n=make_frame(frame,0,p,200,50,1,1,1,4,0);
    mi_feed(&input,frame,6,0); CHECK(input.report.bad_length==1 && input.used==0);
    heartbeat(1,1,2,12); CHECK(input.report.frames==3);
    n=make_frame(frame,0,p,9,50,2,1,1,5,2);
    mi_feed(&input,frame,n,0); CHECK(input.report.unsupported_flags>=1);
    n=make_frame(frame,0,p,9,50,2,1,1,6,1);
    mi_feed(&input,frame,n,0); CHECK(input.report.signed_rejected==1 && input.report.frames==3);
    input.options.allow_signed_unverified=1;
    n=make_frame(frame,0,p,9,50,2,1,1,7,1); mi_feed(&input,frame,n,0);
    CHECK(input.report.frames==4);
    /* MAVLink 2 may omit trailing zero-valued base fields. */
    n=make_frame(frame,0,p,6,50,2,8,1,8,0); mi_feed(&input,frame,n,0);
    CHECK(input.report.frames==5 && input.report.device[1].sysid==8);
    mi_feed(&input,frame,5,0); CHECK(input.used==5); mi_link_reset(&input); CHECK(input.used==0);
    return 1;
}
static int check_navigation_and_named_data(void)
{
    uint8_t p[52]={0};
    initialize(); heartbeat(1,1,2,12);
    navigation(1000,0,1); named(1075,"humidity",50.5f,1); named(1025,"pm25",17.25f,1);
    navigation(1100,1000,1); CHECK(sample_count==0);
    navigation(1300,3000,1);
    CHECK(sample_count==2 && samples[0].sample.t_us==1025000 && samples[1].sample.t_us==1075000);
    CHECK(!strcmp(samples[0].sample.key,"pm25") && !strcmp(samples[1].sample.unit,"unknown"));
    CHECK(samples[0].sample.seq<samples[1].sample.seq);
    CHECK(samples[0].sysid==1 && samples[0].compid==1);
    CHECK(isnan(samples[0].sample.pos_error_m));
    CHECK(samples[0].sample.quality&MI_QUALITY_POSITION_UNKNOWN);
    CHECK(tb_sample_valid(&samples[0].sample));
    CHECK(fabs(samples[0].sample.p[1]-2.77444)<.01);
    CHECK(samples[0].sample.v[0]==20 && samples[0].sample.v[1]==10 && samples[0].sample.v[2]==3);
    CHECK(final_watermark==1100000);
    named(900,"too_late",1,1); CHECK(input.report.late_observation==1);
    named(1350,"other_comp",2,2); CHECK(input.report.wrong_clock_component==1);
    heartbeat(2,1,2,12); navigation(9000,0,2); CHECK(input.report.boot_us<9000000);
    named(1350,"bad key",2,1); CHECK(input.report.invalid_value==1);
    named(1351,"nan",NAN,1); CHECK(input.report.invalid_value==2);
    put64(p,1400000); p[28]=3; put32(p+34,2000); put32(p+38,3000);
    estimator(1400,54,1400);
    feed(24,p,sizeof(p),24,1,1,1400); CHECK(input.report.gps_fix==3);
    navigation(1400,4000,1); named(1450,"temperature",20,1); navigation(1500,5000,1);
    mi_finish(&input,1500); CHECK(sample_count==3 && isfinite(samples[2].sample.pos_error_m));
    CHECK(samples[2].sample.pos_error_m>3.60f && samples[2].sample.pos_error_m<3.70f);
    CHECK(input.pending_count==0);
    return 1;
}
static int check_watermark_boundary(void)
{
    initialize(); heartbeat(1,1,2,12);
    navigation(1000,0,1); navigation(1200,100,1);
    CHECK(input.report.watermark_us==1000000);
    named(1000,"edge",3,1);
    CHECK(sample_count==1 && samples[0].sample.t_us==1000000);
    named(999,"past",1,1); CHECK(input.report.late_observation==1);
    return 1;
}
static int check_finish_and_missing_nav(void)
{
    initialize(); heartbeat(1,1,2,12);
    navigation(1000,0,1); named(1050,"dynamic",4,1); navigation(1100,100,1);
    CHECK(sample_count==0 && input.pending_count==1);
    mi_tick(&input,1251); CHECK(sample_count==1 && input.pending_count==0);
    CHECK(final_watermark==900000); /* host age did not advance boot watermark */
    mi_finish(&input,1300); CHECK(sample_count==1 && final_watermark==1100000);
    initialize(); heartbeat(1,1,2,12); named(1000,"data",1,1);
    mi_tick(&input,4000); CHECK(sample_count==0 && input.report.no_navigation==1 && input.pending_count==0);
    initialize(); heartbeat(1,1,2,12); named(1000,"data",1,1); navigation(1300,0,1);
    CHECK(input.report.no_navigation==1 && sample_count==0);
    return 1;
}
static int check_clocks_and_wrap(void)
{
    uint8_t p[64]={0}; uint64_t old_boot;
    initialize(); heartbeat(1,1,2,12); navigation(10000,0,1); navigation(1000,0,1);
    CHECK(reset_count==1 && input.report.reboots==1 && input.epoch==2);
    initialize(); heartbeat(1,1,2,12); navigation(UINT32_C(0xfffffff0),0,1);
    old_boot=input.report.boot_us; navigation(20,0,1);
    CHECK(reset_count==0 && input.report.boot_us>old_boot);
    CHECK(input.report.boot_us==UINT64_C(4294967316000));
    put64(p,UINT64_C(1800000000000000)); put32(p+8,50);
    feed(2,p,12,137,1,1,200); CHECK(clock_count==1);
    memset(p,0,sizeof(p)); put64(p,UINT64_C(1800000000000000));
    feed(105,p,62,93,1,1,201); CHECK(input.report.no_clock==1);
    return 1;
}
static int check_environment_and_future_fields(void)
{
    uint8_t p[64]={0}, frame[280]; size_t n;
    initialize(); heartbeat(1,1,2,12); navigation(1000,0,1);
    put32(p,1050); putfloat(p+4,1012.25f); put16(p+12,2530);
    feed(29,p,14,115,1,1,1050);
    memset(p,0,sizeof(p)); put32(p,1060); putfloat(p+4,13.5f); memcpy(p+8,"new_key",7); p[21]=17;
    n=make_frame(frame,251,p,22,170,2,1,1,seq++,0); mi_feed(&input,frame,n,1060);
    memset(p,0,sizeof(p)); put64(p,1070000); putfloat(p+44,998.5f); putfloat(p+56,20.5f);
    put16(p+60,(uint16_t)((1u<<9)|(1u<<12))); feed(105,p,62,93,1,1,1070);
    navigation(1100,100,1); mi_finish(&input,1100);
    CHECK(sample_count==5);
    CHECK(!strcmp(samples[0].sample.key,"pressure") && samples[0].sample.value==1012.25);
    CHECK(!strcmp(samples[1].sample.key,"temperature") && fabs(samples[1].sample.value-25.30)<1e-10);
    CHECK(!strcmp(samples[2].sample.key,"new_key") && samples[2].sample.value==13.5);
    CHECK(!strcmp(samples[3].sample.key,"imu_pressure") && samples[3].sample.value==998.5);
    CHECK(!strcmp(samples[4].sample.key,"imu_temperature") && samples[4].sample.value==20.5);
    return 1;
}
static int check_bounded_memory(void)
{
    unsigned j; char key[11];
    initialize(); heartbeat(1,1,2,12);
    for(j=0;j<MI_PENDING_CAP+10;++j) {
        (void)snprintf(key,sizeof(key),"k%u",j); named(1000,key,1,1);
    }
    CHECK(input.pending_count==MI_PENDING_CAP && input.report.pending_overflow==10);
    mi_finish(&input,1001); CHECK(input.pending_count==0 && input.report.no_navigation==MI_PENDING_CAP);
    initialize();
    for(j=1;j<MI_DEVICE_CAP+4;++j) heartbeat((uint8_t)j,1,2,12);
    CHECK(input.report.devices==MI_DEVICE_CAP && input.report.device_overflow==3);
    return 1;
}
static int check_high_rate_navigation(void)
{
    unsigned j;
    initialize(); heartbeat(1,1,2,12);
    for(j=0;j<=200;++j) {
        navigation(1000+j*5,(int32_t)j,1);
        named(1000+j*5,"fast",17,1);
    }
    mi_finish(&input,2001);
    CHECK(sample_count==201 && input.report.no_navigation==0);
    CHECK(input.report.pending_overflow==0);
    for(j=0;j<201;++j) CHECK(samples[j].sample.t_us==1000000+(uint64_t)j*5000);
    return 1;
}
static int check_automatic_origin(void)
{
    mi_options o;
    initialize(); mi_options_default(&o); o.origin_auto=1;
    mi_init(&input,&o,on_event,NULL);
    heartbeat(1,1,2,12); navigation(1000,0,1); named(1000,"home",1,1);
    navigation(1100,1000,1); mi_finish(&input,1100);
    CHECK(origin_count==1 && observed_origin[0]==37 && observed_origin[1]==127 && observed_origin[2]==100);
    CHECK(sample_count==1 && samples[0].sample.p[0]==0 && samples[0].sample.p[1]==0 && samples[0].sample.p[2]==0);
    CHECK(input.options.origin_lat==37 && input.options.origin_lon==127);
    navigation(10000,1000,1); navigation(1000,1000,1);
    CHECK(reset_count==1 && origin_count==2 && observed_origin[0]==37);
    named(1000,"home",2,1); mi_finish(&input,1100);
    CHECK(sample_count==2 && samples[1].sample.p[1]>11 && samples[1].sample.p[1]<12);
    return 1;
}
static int check_estgps_trapezoid_and_weather(void)
{
    trusted_anchor(1000,0,0,0);
    CHECK(input.report.have_gps_anchor && input.report.gps_fix_fresh);
    CHECK(input.report.position_kind==WEATHER_POSITION_GPS && input.report.estimation_age_us==0);
    CHECK(input.report.last_fix_boot_us==1000000 && isfinite(input.report.navigation_error_m));
    gps(1,1100);
    local_velocity(1200,2,4,-1,1200);
    local_velocity(1400,4,8,-2,1400);
    local_velocity(1600,6,12,-3,1600);
    CHECK(input.report.have_navigation && input.report.position_kind==WEATHER_POSITION_ESTGPS);
    CHECK(input.report.estimate_source==MI_ESTIMATE_VELOCITY && input.report.estimation_age_us==600000);
    CHECK(isnan(input.report.navigation_error_m));
    CHECK(fabs(input.estimate_p[0]-3.6)<1e-12 && fabs(input.estimate_p[1]-1.8)<1e-12);
    CHECK(fabs(input.estimate_p[2]-.9)<1e-12 && input.report.altitude_msl_m>100.89);
    CHECK(input.report.lat>37 && input.report.lon>127);
    local_velocity(1600,100,100,100,1601); /* same boot timestamp cannot integrate twice */
    CHECK(fabs(input.estimate_p[0]-3.6)<1e-12);
    atmospheric(1600000,1602); local_velocity(1800,6,12,-3,1802); mi_finish(&input,1803);
    CHECK(weather_count==2 && weather_samples[0].has_position && weather_samples[1].has_position);
    CHECK(weather_samples[0].position_kind==WEATHER_POSITION_ESTGPS && weather_samples[0].estimation_age_us==600000);
    CHECK((weather_samples[0].quality&MI_QUALITY_ESTGPS) && isnan(weather_samples[0].pos_error_m));
    CHECK(fabs(weather_samples[0].p[0]-3.6)<1e-12 && weather_samples[0].t_us==1600000);
    CHECK(weather_samples[0].t_us==weather_samples[1].t_us && weather_samples[1].position_kind==WEATHER_POSITION_ESTGPS);
    CHECK(fabs(weather_samples[0].value-293.15)<1e-12 && weather_samples[1].value==.5);
    estimator(2000,54,2000); gps(3,2000); navigation(2000,1000,1);
    CHECK(input.report.position_kind==WEATHER_POSITION_GPS && input.report.estimation_age_us==0);
    CHECK(input.report.estimate_source==MI_ESTIMATE_EKF && input.report.last_fix_boot_us==2000000);
    CHECK(fabs(input.estimate_p[0])<1e-12 && input.estimate_p[1]>11 && input.estimate_p[1]<12);
    atmospheric(2000000,2000); mi_finish(&input,2001);
    CHECK(weather_count==4 && weather_samples[2].position_kind==WEATHER_POSITION_GPS);
    CHECK(weather_samples[2].estimation_age_us==0 && !(weather_samples[2].quality&MI_QUALITY_ESTGPS));
    return 1;
}
static int check_estgps_ekf_preference_and_partial_anchor(void)
{
    trusted_anchor(1000,0,0,0); gps(1,1100);
    local_velocity(1200,2,4,-1,1200); navigation(1250,1000,1);
    CHECK(input.report.position_kind==WEATHER_POSITION_ESTGPS && input.report.estimate_source==MI_ESTIMATE_EKF);
    CHECK(input.report.estimation_age_us==250000 && fabs(input.report.lat-37.0001)<1e-12);
    CHECK(isnan(input.report.navigation_error_m));
    local_velocity(1400,4,8,-2,1400);
    CHECK(input.report.estimate_source==MI_ESTIMATE_EKF && input.report.navigation_boot_us==1250000);
    CHECK(input.estimate_p[1]>11.5 && input.estimate_p[1]<12); /* only 1250..1400 integrated */
    /* No synthetic local x/y/z was copied into the global ENU frame. */
    CHECK(fabs(input.estimate_p[0])<2 && fabs(input.estimate_p[2])<1);
    local_velocity(1600,6,12,-3,1600); local_velocity(1800,8,16,-4,1800);
    CHECK(input.report.estimate_source==MI_ESTIMATE_VELOCITY && input.report.navigation_boot_us==1800000);
    mi_tick(&input,5000); CHECK(!input.report.have_navigation);
    navigation(1300,2000,1); /* Reordered EKF data cannot resurrect an older displayed position. */
    CHECK(!input.report.have_navigation && input.report.navigation_boot_us==1800000);
    return 1;
}
static int check_estgps_missing_evidence(void)
{
    unsigned j;
    initialize(); heartbeat(1,1,2,12); estimator(1000,6,1000);
    for(j=0;j<5;++j) local_velocity(1000+j*200,5,10,-1,1000+j*200);
    CHECK(!input.report.have_navigation && !input.report.have_gps_anchor);
    CHECK(input.report.position_flags&MI_ESTGPS_NO_ANCHOR);
    /* GPS fix without absolute estimator position validity cannot anchor. */
    gps(3,1800); navigation(1800,0,1);
    CHECK(!input.report.have_gps_anchor && !input.report.have_navigation && input.report.position_kind==WEATHER_POSITION_UNKNOWN);
    CHECK(input.report.estimation_age_us==UINT32_MAX && isnan(input.report.navigation_error_m));
    local_velocity(2000,5,10,-1,2000); local_velocity(2200,5,10,-1,2200); local_velocity(2400,5,10,-1,2400);
    CHECK(!input.report.have_navigation && input.report.estimate_source!=MI_ESTIMATE_VELOCITY);
    /* No estimator status at all: a legacy EKF position remains explicitly
     * estimated, but neither a GPS anchor nor receiver-side integration exists. */
    initialize(); heartbeat(1,1,2,12); gps(3,1000); navigation(1000,0,1);
    local_velocity(1200,1,2,3,1200); local_velocity(1600,1,2,3,1600);
    CHECK(!input.report.have_gps_anchor && !input.report.have_navigation);
    CHECK(input.report.position_flags&MI_ESTGPS_STATUS_INVALID);
    /* Existing anchor without any later ground velocity cannot move itself. */
    trusted_anchor(1000,0,0,0); gps(1,1100); mi_tick(&input,1600);
    CHECK(!input.report.have_navigation && input.report.estimate_source==MI_ESTIMATE_EKF);
    CHECK(input.estimate_p[0]==0 && input.estimate_p[1]==0);
    return 1;
}
static int check_estgps_validity_and_gaps(void)
{
    const uint16_t bad_flags[]={52,50,182,1078,2102}; unsigned j;
    for(j=0;j<sizeof(bad_flags)/sizeof(bad_flags[0]);++j) {
        trusted_anchor(1000,1,2,-3); gps(1,1100); estimator(1200,bad_flags[j],1200);
        local_velocity(1200,1,2,-3,1200); local_velocity(1600,1,2,-3,1600);
        CHECK(!input.report.have_navigation && !input.estimate_active);
        CHECK(input.report.position_flags&MI_ESTGPS_STATUS_INVALID);
    }
    trusted_anchor(1000,1,2,-3); gps(1,1100);
    local_velocity(1200,1,NAN,-3,1200); local_velocity(1600,1,2,-3,1600);
    CHECK(!input.report.have_navigation && !input.estimate_active);
    CHECK(input.report.position_flags&MI_ESTGPS_VELOCITY_INVALID);
    trusted_anchor(1000,1,2,-3); gps(1,1100);
    local_velocity(1200,0,501,0,1200); CHECK(!input.estimate_active);
    trusted_anchor(1000,1,2,-3); gps(1,1100);
    local_velocity(1200,1,2,-3,1200); local_velocity(1800,1,2,-3,1800); local_velocity(2000,1,2,-3,2000);
    CHECK(!input.report.have_navigation && !input.estimate_active);
    CHECK(input.report.position_flags&MI_ESTGPS_VELOCITY_GAP);
    /* Host freshness is required independently of short source intervals. */
    trusted_anchor(1000,1,2,-3); gps(1,1100);
    estimator(1200,54,1800); local_velocity(1200,1,2,-3,1800);
    CHECK(!input.estimate_active && (input.report.position_flags&MI_ESTGPS_VELOCITY_GAP));
    /* A stale validity status is not made fresh by receiving new velocities. */
    trusted_anchor(1000,1,2,-3); gps(1,1100);
    for(j=1;j<=11;++j) local_velocity(1000+j*200,1,2,-3,1000+j*200);
    CHECK(!input.report.have_navigation && (input.report.position_flags&MI_ESTGPS_STATUS_INVALID));
    /* Raw fix 255/9 are unknown future enums, not proof of a GPS fix. */
    trusted_anchor(1000,0,0,0); gps(255,1100); CHECK(!input.report.gps_fix_fresh);
    gps(9,1101); CHECK(!input.report.gps_fix_fresh);
    for(j=3;j<=8;++j) { gps((uint8_t)j,1102+j); CHECK(input.report.gps_fix_fresh); }
    mi_tick(&input,3110); CHECK(input.report.gps_fix_fresh);
    mi_tick(&input,3111); CHECK(!input.report.gps_fix_fresh);
    return 1;
}
static int check_estgps_expiry_and_resets(void)
{
    unsigned j;
    trusted_anchor(1000,1,2,-.5f); gps(1,1100);
    for(j=1;j<=150;++j) {
        uint32_t t=1000+j*200;
        if(j%5==0) estimator(t,6,t);
        local_velocity(t,1,2,-.5f,t);
    }
    CHECK(input.report.have_navigation && input.report.position_kind==WEATHER_POSITION_ESTGPS);
    CHECK(input.report.estimation_age_us==30000000 && input.report.navigation_boot_us==31000000);
    CHECK(fabs(input.estimate_p[0]-60)<1e-10 && fabs(input.estimate_p[1]-30)<1e-10);
    local_velocity(31200,1,2,-.5f,31200);
    CHECK(!input.report.have_navigation && (input.report.position_flags&MI_ESTGPS_EXPIRED));
    trusted_anchor(1000,1,2,3); gps(1,1100); estimator(1200,6,32001); local_velocity(1200,1,2,3,32001);
    CHECK(!input.report.have_navigation && (input.report.position_flags&MI_ESTGPS_EXPIRED));
    trusted_anchor(10000,1,2,3); local_velocity(1000,1,2,3,1000);
    CHECK(reset_count==1 && !input.report.have_gps_anchor && !input.report.have_navigation);
    CHECK(input.options.origin_lat==37 && input.options.origin_lon==127);
    trusted_anchor(1000,1,2,3); mi_finish(&input,1001); mi_link_reset(&input);
    CHECK(!input.report.have_gps_anchor && !input.report.have_navigation && input.nav_count==0);
    CHECK(input.report.position_kind==WEATHER_POSITION_UNKNOWN && input.report.estimation_age_us==UINT32_MAX);
    CHECK(input.options.origin_lat==37 && input.options.origin_lon==127 && input.report.selected_system==1);
    estimator(1200,54,1200); local_velocity(1200,1,2,3,1200); local_velocity(1600,1,2,3,1600);
    CHECK(!input.report.have_navigation && !input.report.have_gps_anchor);
    return 1;
}
static int check_estgps_status_is_not_clock_owner(void)
{
    uint32_t epoch;
    trusted_anchor(100000,1,2,-3); atmospheric(100000000,100000); named(100000,"keep",1,1);
    epoch=input.epoch;
    CHECK(input.weather_pending_count==2 && input.pending_count==1);
    estimator(90000,0,100001); /* 10 s delayed status cannot reset the live 100 s source. */
    CHECK(input.epoch==epoch && reset_count==0 && input.report.reboots==0);
    CHECK(input.have_gps_anchor && input.gps_anchor_us==100000000 && input.nav_count==1);
    CHECK(input.estimator_boot_us==100000000 && input.estimator_flags==54);
    CHECK(input.weather_pending_count==2 && input.pending_count==1);
    estimator(110000,0,100002); estimator(99900,0,100003);
    CHECK(input.epoch==epoch && input.estimator_boot_us==100000000 && input.estimator_flags==54);
    CHECK(input.weather_pending_count==2 && input.pending_count==1);
    {
        uint8_t p[42]={0}; put64(p,UINT64_C(1800000000000000));
        feed(230,p,sizeof(p),163,1,1,100004);
        CHECK(input.report.no_clock==1 && input.report.reboots==0 && input.have_gps_anchor);
    }
    initialize(); heartbeat(1,1,2,12); estimator(100000,54,1000);
    CHECK(!input.have_boot); /* Status cannot establish even the initial source clock. */
    navigation(1000,0,1); estimator(1000,54,1001); gps(3,1002); navigation_at(1100,0,1,1100);
    CHECK(input.estimator_boot_us==1000000 && input.report.have_gps_anchor && input.report.reboots==0);
    return 1;
}
static int check_estgps_stale_and_known_invalid_global(void)
{
    const uint16_t bad_flags[]={6,22,38,1078,2102}; unsigned j;
    trusted_anchor(1000,0,0,0); gps(3,1600);
    CHECK(input.report.gps_fix_fresh && !input.report.have_navigation);
    CHECK(input.report.position_kind==WEATHER_POSITION_UNKNOWN && input.report.navigation_boot_us==1000000);
    trusted_anchor(1000,0,0,0); estimator(1200,182,1200); gps(3,1200); navigation(1200,0,1);
    CHECK(input.report.have_navigation && input.report.position_kind==WEATHER_POSITION_GPS && input.have_gps_anchor);
    CHECK(!input.estimate_active); /* at-rest absolute position is valid; router DR is forbidden */
    for(j=0;j<sizeof(bad_flags)/sizeof(bad_flags[0]);++j) {
        trusted_anchor(1000,0,0,0); estimator(1200,bad_flags[j],1200); navigation(1200,1000,1);
        CHECK(!input.report.have_navigation && input.report.position_kind==WEATHER_POSITION_UNKNOWN);
        CHECK(input.nav_count==1 && input.report.nav_rejected==1);
        CHECK(input.report.position_flags&MI_ESTGPS_STATUS_INVALID);
        atmospheric(1200000,1200); mi_finish(&input,1201);
        CHECK(weather_count==2 && !weather_samples[0].has_position && !weather_samples[1].has_position);
        CHECK(weather_samples[0].position_kind==WEATHER_POSITION_UNKNOWN && weather_samples[0].estimation_age_us==UINT32_MAX);
    }
    return 1;
}
static int check_estgps_local_before_delayed_global(void)
{
    trusted_anchor(100000,0,0,0);
    local_velocity(100050,2,4,0,100050);
    navigation_at(100000,0,1,100060); /* timestamp is bracketed by the last two LOCAL velocities */
    CHECK(input.estimate_active && input.estimate_boot_us==100050000);
    CHECK(fabs(input.estimate_p[0]-.1)<1e-12 && fabs(input.estimate_p[1]-.05)<1e-12);
    local_velocity(100150,6,12,0,100150);
    CHECK(input.estimate_active && fabs(input.estimate_p[0]-.9)<1e-12 && fabs(input.estimate_p[1]-.45)<1e-12);
    gps(1,100200); local_velocity(100350,6,12,0,100350);
    local_velocity(100550,6,12,0,100550); local_velocity(100750,6,12,0,100750);
    CHECK(input.report.have_navigation && input.report.estimate_source==MI_ESTIMATE_VELOCITY);
    CHECK(fabs(input.estimate_p[0]-8.1)<1e-11 && fabs(input.estimate_p[1]-4.05)<1e-11);
    return 1;
}
static int check_estgps_raw_gps_source_clock(void)
{
    uint8_t p[52]={0}; uint32_t epoch;
    trusted_anchor(100000,0,0,0); epoch=input.epoch;
    p[28]=3; put32(p+34,2000); put32(p+38,3000);
    put64(p,90000000); feed(24,p,sizeof(p),24,1,1,100010);
    CHECK(!input.report.gps_fix_fresh && input.report.gps_fix==3 && input.have_gps_anchor);
    estimator(100100,54,100100); navigation(100100,1000,1);
    CHECK(input.report.position_kind==WEATHER_POSITION_ESTGPS && input.gps_anchor_us==100000000);
    CHECK(input.report.reboots==0 && input.epoch==epoch);
    put64(p,UINT64_C(1800000000000000)); feed(24,p,sizeof(p),24,1,1,100110);
    navigation(100200,1000,1);
    CHECK(!input.report.gps_fix_fresh && input.gps_anchor_us==100000000 && input.report.position_kind==WEATHER_POSITION_ESTGPS);
    put64(p,0); feed(24,p,sizeof(p),24,1,1,100210);
    CHECK(!input.report.gps_fix_fresh && !input.have_gps_fix_clock);
    put64(p,110000000); feed(24,p,sizeof(p),24,1,1,100220);
    CHECK(!input.report.gps_fix_fresh && !input.have_gps_fix_clock && input.report.reboots==0);
    put64(p,100300000); feed(24,p,sizeof(p),24,1,1,100290);
    navigation(100300,1000,1);
    CHECK(input.report.gps_fix_fresh && input.report.position_kind==WEATHER_POSITION_GPS);
    CHECK(input.gps_anchor_us==100300000 && input.report.estimation_age_us==0);
    /* A raw fix may not alter the source boot clock or reset its session. */
    CHECK(input.high_boot_us==100300000 && input.epoch==epoch && input.report.reboots==0);
    return 1;
}
static int check_estgps_invalid_interval_and_origin(void)
{
    uint8_t p[28]={0}; mi_options o;
    trusted_anchor(1000,0,0,0);
    estimator(1100,6,1100); navigation(1100,1000,1);
    atmospheric(1125000,1125);
    estimator(1200,54,1200); gps(3,1200); navigation(1200,2000,1);
    CHECK(input.nav_count==2 && input.nav[1].t_us-input.nav[0].t_us==TB_MAX_NAV_AGE_US);
    CHECK(input.nav_info[0].segment!=input.nav_info[1].segment);
    mi_tick(&input,1325);
    CHECK(weather_count==2 && !weather_samples[0].has_position && !weather_samples[1].has_position);
    CHECK(weather_samples[0].position_kind==WEATHER_POSITION_UNKNOWN && weather_samples[0].estimation_age_us==UINT32_MAX);
    /* Repeated ABS-invalid GPI must not erase valid ground-velocity history.
     * Both bracketing points were produced during the same invalid interval. */
    trusted_anchor(1000,1,2,0); gps(1,1100);
    estimator(1200,6,1200); local_velocity(1200,1,2,0,1200); navigation(1200,1000,1);
    local_velocity(1400,1,2,0,1400); atmospheric(1500000,1500);
    estimator(1600,6,1600); navigation(1600,2000,1); local_velocity(1600,1,2,0,1600);
    mi_tick(&input,1700);
    CHECK(weather_count==2 && weather_samples[0].has_position && weather_samples[1].has_position);
    CHECK(weather_samples[0].position_kind==WEATHER_POSITION_ESTGPS && isnan(weather_samples[0].pos_error_m));
    CHECK(fabs(weather_samples[0].p[0]-1.0)<1e-12 && fabs(weather_samples[0].p[1]-.5)<1e-12);
    initialize(); mi_options_default(&o); o.origin_auto=1; mi_init(&input,&o,on_event,NULL);
    heartbeat(1,1,2,12); estimator(1100,6,1100); put32(p,1100);
    feed(33,p,sizeof(p),104,1,1,1100); /* invalid startup GPI contains 0,0 */
    CHECK(!input.origin_set && origin_count==0 && !input.report.have_navigation);
    estimator(1200,54,1200); gps(3,1200); navigation(1200,0,1);
    CHECK(input.origin_set && origin_count==1 && observed_origin[0]==37 && observed_origin[1]==127);
    CHECK(input.options.origin_lat==37 && input.options.origin_lon==127);
    CHECK(input.report.position_kind==WEATHER_POSITION_GPS && input.nav_count==1);
    CHECK(input.nav[0].p[0]==0 && input.nav[0].p[1]==0 && input.nav[0].p[2]==0);
    return 1;
}
static double seconds(void)
{
#ifdef _WIN32
    LARGE_INTEGER now,hz; QueryPerformanceFrequency(&hz); QueryPerformanceCounter(&now);
    return (double)now.QuadPart/(double)hz.QuadPart;
#else
    return (double)clock()/CLOCKS_PER_SEC;
#endif
}
static int benchmark(void)
{
    unsigned j; double start,elapsed;
    initialize(); heartbeat(1,1,2,12); start=seconds();
    for(j=0;j<100000;++j) {
        if(j%10==0) navigation(1000+j,0,1);
        named(1000+j,"sensor",17,1);
    }
    navigation(101000,0,1); mi_finish(&input,101000); elapsed=seconds()-start;
    printf("{\"scenario\":\"MAVLink_1000Hz_named_100Hz_navigation\",\"observations\":%llu,\"seconds\":%.9g,\"us_per_observation\":%.9g,\"decoder_bytes\":%zu,\"pending_overflow\":%llu,\"no_navigation\":%llu}\n",
       (unsigned long long)sample_count,elapsed,elapsed*1e6/100000,sizeof(input),
       (unsigned long long)input.report.pending_overflow,(unsigned long long)input.report.no_navigation);
    return sample_count==100000 && !input.report.pending_overflow && !input.report.no_navigation && !failures;
}
static int benchmark_estgps(void)
{
    unsigned j; double start, elapsed;
    trusted_anchor(1000,1,2,-.5f); start=seconds();
    for(j=1;j<=100000;++j) {
        uint32_t t=1000+j*20;
        if(j%50==0) estimator(t,54,t);
        if(j%100==0) { gps(3,t); navigation(t,0,1); }
        local_velocity(t,1,2,-.5f,t);
    }
    elapsed=seconds()-start;
    printf("{\"scenario\":\"ESTGPS_50Hz_LOCAL_velocity\",\"velocity_packets\":100000,\"seconds\":%.9g,\"us_per_velocity_packet\":%.9g,\"decoder_bytes\":%zu,\"nav_rejected\":%llu}\n",
        elapsed,elapsed*10,sizeof(input),(unsigned long long)input.report.nav_rejected);
    return input.report.have_gps_anchor && !input.report.nav_rejected && !failures;
}
int main(int argc, char **argv)
{
    if(argc==2 && !strcmp(argv[1],"--bench-estgps")) return benchmark_estgps()?0:1;
    if(argc==2 && !strcmp(argv[1],"--bench")) return benchmark()?0:1;
    if(argc!=1) return 2;
    if(!check_discovery_and_fragmentation() || !check_parser_recovery() ||
       !check_navigation_and_named_data() || !check_finish_and_missing_nav() || !check_watermark_boundary() ||
       !check_clocks_and_wrap() || !check_environment_and_future_fields() ||
       !check_bounded_memory() || !check_high_rate_navigation() || !check_automatic_origin() ||
       !check_estgps_trapezoid_and_weather() || !check_estgps_ekf_preference_and_partial_anchor() ||
       !check_estgps_missing_evidence() || !check_estgps_validity_and_gaps() || !check_estgps_expiry_and_resets() ||
       !check_estgps_status_is_not_clock_owner() || !check_estgps_stale_and_known_invalid_global() ||
       !check_estgps_local_before_delayed_global() || !check_estgps_raw_gps_source_clock() ||
       !check_estgps_invalid_interval_and_origin() || failures) return 1;
    printf("PASS %u MAVLink wire checks\n",checks); return 0;
}