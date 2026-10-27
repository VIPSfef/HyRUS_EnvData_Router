#include "spatial.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static sp_state state;
static sp_batch batch;
static char json[512u * 1024u];
static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); } } while (0)
#define NEAR(a,b,e) CHECK(fabs((double)(a)-(double)(b)) <= (e))

static weather_sample sample(uint64_t elapsed_us,double x,float vx)
{
    weather_sample w;
    memset(&w,0,sizeof(w));
    w.t_us=UINT64_C(1000000)+elapsed_us; w.seq=(uint32_t)(elapsed_us/1000);
    w.metric=WEATHER_TEMPERATURE; w.message_id=8009; w.sensor_id=3;
    w.has_time=w.has_position=w.has_velocity=1; w.position_kind=WEATHER_POSITION_GPS;
    w.value=280.0+(double)elapsed_us*1e-8; w.measurement_variance=NAN;
    w.pos_error_m=NAN; w.time_uncert_us=UINT32_MAX;
    w.p[0]=x; w.p[1]=10; w.p[2]=100; w.v[0]=vx;
    w.navigation_segment=1;
    return w;
}
static sp_options options(double step,int adaptive)
{
    sp_options o; sp_options_default(&o); o.step_m=step; o.adaptive=adaptive; return o;
}
static void test_options(void)
{
    sp_options o; sp_options_default(&o);
    CHECK(o.step_m==10 && o.adaptive==1 && o.gap_us==2000000);
    CHECK(sp_options_valid(&o));
    o.step_m=NAN; CHECK(!sp_options_valid(&o));
    sp_init(&state,&o); CHECK(state.options.step_m==10);
    o=options(0,0); CHECK(!sp_set_options(&state,&o)); CHECK(state.options.step_m==10);
    o=options(1,0); CHECK(sp_set_options(&state,&o)); CHECK(state.options.step_m==1);
    o.gap_us=0; CHECK(!sp_options_valid(&o));
    o=options(SP_MAX_WIDTH_M+1,1); CHECK(!sp_options_valid(&o));
    sp_init(NULL,NULL); sp_break(NULL,0); sp_take_snapshot(NULL,&batch);
    sp_prepare_snapshot(NULL,&batch); sp_prepare_snapshot(&state,NULL); sp_commit_snapshot(NULL);
    CHECK(!sp_add(NULL,NULL,0,0));
}
static void test_uniform(double speed,unsigned expected_n)
{
    sp_options o=options(10,1);
    unsigned i; double mean,m2;
    sp_init(&state,&o);
    for(i=0;i<=expected_n;++i) {
        weather_sample w=sample((uint64_t)i*10000,speed*i*.01,(float)speed);
        CHECK(sp_add(&state,&w,1,1));
    }
    sp_take_snapshot(&state,&batch);
    CHECK(batch.n==2); CHECK(batch.record[0].status==SP_COMPLETE);
    CHECK(batch.record[0].n==expected_n && batch.record[1].n==1);
    CHECK(batch.record[0].bin_index==0 && batch.record[1].bin_index==1);
    CHECK(batch.record[0].width_m==10 && batch.record[1].width_m==10);
    mean=280+(expected_n-1)*.0001*.5;
    m2=.0001*.0001*(double)expected_n*((double)expected_n*expected_n-1)/12;
    NEAR(batch.record[0].mean,mean,1e-10); NEAR(batch.record[0].m2,m2,1e-8);
    NEAR(batch.record[0].s_start_m,0,1e-12);
    NEAR(batch.record[0].s_end_m,10-speed*.01,1e-9);
    NEAR(batch.report.s_m,10,1e-9);
    CHECK(batch.record[0].representative.t_us==1000000);
    CHECK(batch.report.spatial_samples==expected_n+1);
}
static void test_pair_and_revisions(void)
{
    sp_options o=options(1,0);
    weather_sample w=sample(0,0,1);
    uint64_t revision;
    unsigned i;
    sp_init(&state,&o);
    for(i=0;i<50;++i) {
        w=sample((uint64_t)i*10000,i*.01,1); CHECK(sp_add(&state,&w,77,1));
        w.metric=WEATHER_HUMIDITY; w.value=.5; CHECK(sp_add(&state,&w,77,1));
    }
    NEAR(state.s_m,.49,1e-12); sp_take_snapshot(&state,&batch);
    CHECK(batch.n==2 && batch.record[0].n==50 && batch.record[1].n==50);
    revision=batch.record[0].revision;
    sp_take_snapshot(&state,&batch); CHECK(batch.n==0); CHECK(state.open[0].record.n==50);
    for(i=50;i<=100;++i) {
        w=sample((uint64_t)i*10000,i*.01,1); CHECK(sp_add(&state,&w,77,1));
        w.metric=WEATHER_HUMIDITY; w.value=.5; CHECK(sp_add(&state,&w,77,1));
    }
    sp_take_snapshot(&state,&batch);
    CHECK(batch.n==4); CHECK(batch.record[0].status==SP_COMPLETE && batch.record[0].n==100);
    CHECK(batch.record[0].revision>revision);
    CHECK(batch.record[1].status==SP_COMPLETE && batch.record[1].n==100);
    CHECK(batch.record[2].n==1 && batch.record[3].n==1);
    NEAR(batch.report.s_m,1,1e-12);
    revision=batch.record[2].revision;
    sp_break(&state,SP_FLAG_CONTEXT_CHANGE); sp_take_snapshot(&state,&batch);
    CHECK(batch.n==2 && batch.record[0].status==SP_INTERRUPTED);
    CHECK(batch.record[0].revision==revision+1); CHECK(batch.record[0].flags&SP_FLAG_CONTEXT_CHANGE);
}
static void test_transactional_snapshot(void)
{
    static sp_state before;
    static sp_batch prepared;
    sp_options o=options(1,0);
    weather_sample w;
    unsigned i;
    sp_init(&state,&o);
    /* Include completed bins and dirty partial revisions in one transaction. */
    for(i=0;i<=1;++i) {
        w=sample((uint64_t)i*1000000,(double)i,1);
        CHECK(sp_add(&state,&w,77,1));
        w.metric=WEATHER_HUMIDITY; w.value=.5;
        CHECK(sp_add(&state,&w,77,1));
    }
    CHECK(state.completed_n==2 && state.open_n==2);
    before=state;
    sp_prepare_snapshot(&state,&prepared);
    CHECK(prepared.n==4 && prepared.record[0].status==SP_COMPLETE && prepared.record[2].status==SP_PARTIAL);
    CHECK(!memcmp(&state,&before,sizeof(state)));
    /* Simulate failed storage: retry without a commit must reproduce all data. */
    sp_prepare_snapshot(&state,&batch);
    CHECK(!memcmp(&batch,&prepared,sizeof(batch)));
    CHECK(!memcmp(&state,&before,sizeof(state)));
    CHECK(sp_batch_json(&batch,json,sizeof(json))>0);
    sp_commit_snapshot(&state);
    CHECK(!state.completed_n && state.completed_head==2 && state.open_n==2);
    CHECK(state.open[0].emitted_revision==state.open[0].record.revision);
    CHECK(state.open[1].emitted_revision==state.open[1].record.revision);
    CHECK(state.open[0].record.n==1 && state.open[1].record.n==1);
    sp_prepare_snapshot(&state,&batch); CHECK(batch.n==0);
    /* Future updates still create a fresh cumulative revision after the commit. */
    w=sample(1100000,1.1,1); CHECK(sp_add(&state,&w,77,1));
    sp_prepare_snapshot(&state,&batch);
    CHECK(batch.n==1 && batch.record[0].n==2);
    CHECK(batch.record[0].revision>prepared.record[2].revision);
    CHECK(prepared.n==4 && prepared.record[2].n==1);
    sp_take_snapshot(&state,&batch);
    CHECK(batch.n==1 && state.open[0].emitted_revision==batch.record[0].revision);
    sp_prepare_snapshot(&state,&batch); CHECK(batch.n==0);
}
static void test_stationary(void)
{
    sp_options o=options(1,1); unsigned i;
    sp_init(&state,&o);
    for(i=0;i<1001;++i) {
        weather_sample w=sample((uint64_t)i*10000,.1*sin(i),0);
        CHECK(sp_add(&state,&w,1,1));
    }
    sp_take_snapshot(&state,&batch);
    CHECK(batch.n==1 && batch.record[0].status==SP_PARTIAL && batch.record[0].n==1001);
    CHECK(batch.report.s_m==0 && batch.report.completed_records==0 && batch.report.inconsistent_steps==0);
    CHECK(batch.record[0].s_start_m==0 && batch.record[0].s_end_m==0);
}
static void test_unknown_breaks(void)
{
    sp_options o=options(1,0); unsigned kind;
    for(kind=0;kind<4;++kind) {
        weather_sample w=sample(0,0,1);
        sp_init(&state,&o); CHECK(sp_add(&state,&w,1,1));
        w=sample(100000,.1,1);
        if(kind==0) { w.has_velocity=w.has_position=0; w.position_kind=WEATHER_POSITION_UNKNOWN; }
        if(kind==1) { w.has_velocity=0; }
        if(kind==2) { w.has_velocity=w.has_position=w.has_time=0; w.position_kind=WEATHER_POSITION_UNKNOWN; }
        CHECK(!sp_add(&state,&w,1,kind==3 ? 0 : 1));
        CHECK(!state.have_step && state.open_n==0);
        w=sample(200000,.2,1); CHECK(sp_add(&state,&w,1,1));
        CHECK(state.path_id==2 && state.s_m==0);
        sp_take_snapshot(&state,&batch);
        CHECK(batch.n==2 && batch.record[0].status==SP_INTERRUPTED && batch.record[1].status==SP_PARTIAL);
        CHECK(batch.record[0].n==1 && batch.record[1].n==1);
        CHECK(batch.report.unknown_samples==1);
    }
    {
        weather_sample w=sample(0,0,0);
        sp_init(&state,&o); w.has_position=w.has_velocity=0; w.position_kind=WEATHER_POSITION_UNKNOWN;
        CHECK(!sp_add(&state,&w,1,0)); sp_take_snapshot(&state,&batch);
        CHECK(batch.n==0 && batch.report.path_id==0 && !batch.report.path_known);
    }
}
static void test_continuity(void)
{
    sp_options o=options(10,0); weather_sample w;
    sp_init(&state,&o);
    w=sample(0,0,1); CHECK(sp_add(&state,&w,1,1));
    w=sample(100000,.1,1); CHECK(sp_add(&state,&w,2,1));
    CHECK(state.path_id==2 && state.s_m==0);
    w=sample(200000,.2,1); w.position_kind=WEATHER_POSITION_ESTGPS; CHECK(sp_add(&state,&w,2,1));
    CHECK(state.path_id==3 && state.s_m==0);
    w=sample(2300001,2.300001,1); w.position_kind=WEATHER_POSITION_ESTGPS; CHECK(sp_add(&state,&w,2,1));
    CHECK(state.path_id==4 && state.s_m==0);
    w=sample(100,0,1); CHECK(sp_add(&state,&w,3,1)); /* reboot context, even with smaller clock */
    CHECK(state.path_id==5 && state.s_m==0);
    sp_take_snapshot(&state,&batch);
    CHECK(batch.n==5); CHECK(batch.record[0].flags&SP_FLAG_CONTEXT_CHANGE);
    CHECK(batch.record[1].flags&SP_FLAG_SOURCE_CHANGE); CHECK(batch.record[2].flags&SP_FLAG_TIME_GAP);
    CHECK(batch.record[2].source_kind==WEATHER_POSITION_ESTGPS);
    w=sample(50,0,1); CHECK(!sp_add(&state,&w,3,1));
    CHECK(state.report.late_samples==1 && state.path_id==5 && state.open[0].record.n==1);
    o.step_m=20; CHECK(sp_set_options(&state,&o)); CHECK(!state.have_step);
    w=sample(200,.0001,1); CHECK(sp_add(&state,&w,3,1)); CHECK(state.width_m==20);
}
static void test_same_time_conflict(void)
{
    sp_options o=options(10,0); weather_sample w=sample(0,0,1);
    sp_init(&state,&o); CHECK(sp_add(&state,&w,1,1));
    w.p[0]=1; w.metric=WEATHER_HUMIDITY; w.value=.5;
    CHECK(!sp_add(&state,&w,1,1)); CHECK(!state.have_step);
    w=sample(100000,.1,1); CHECK(sp_add(&state,&w,1,1));
    CHECK(state.path_id==2 && state.s_m==0);
}
static void test_disagreement(void)
{
    sp_options o=options(10,0); weather_sample w;
    sp_init(&state,&o); w=sample(0,0,0); CHECK(sp_add(&state,&w,1,1));
    w=sample(100000,5,0); CHECK(!sp_add(&state,&w,1,1));
    CHECK(state.report.inconsistent_steps==1 && !state.report.path_known);
    w.metric=WEATHER_HUMIDITY; w.value=.5; CHECK(!sp_add(&state,&w,1,1));
    w=sample(200000,5,0); CHECK(sp_add(&state,&w,1,1));
    sp_take_snapshot(&state,&batch); CHECK(batch.n==2);
    CHECK(batch.record[0].status==SP_INTERRUPTED && batch.record[0].flags&SP_FLAG_MOTION_UNKNOWN);
    CHECK(batch.record[1].s_start_m==0 && batch.record[1].n==1);
}
static void test_undersampling(void)
{
    sp_options o=options(10,0); weather_sample w;
    sp_init(&state,&o); w=sample(0,0,100); CHECK(sp_add(&state,&w,1,1));
    w=sample(500000,50,100); CHECK(sp_add(&state,&w,1,1)); sp_take_snapshot(&state,&batch);
    CHECK(batch.n==2 && batch.record[0].n==1 && batch.record[1].n==1);
    CHECK(batch.record[1].bin_index==5 && batch.record[1].interval_start_m==50 && batch.record[1].interval_end_m==60);
    CHECK(batch.record[1].s_start_m==50 && batch.record[1].s_end_m==50);
    CHECK(batch.record[0].flags&SP_FLAG_UNDERSAMPLED); CHECK(batch.record[1].flags&SP_FLAG_UNDERSAMPLED);
    CHECK(batch.report.undersampled_steps==1 && batch.report.completed_records==1);
    o=options(10,1); sp_init(&state,&o);
    w=sample(0,0,250); CHECK(sp_add(&state,&w,1,1));
    w=sample(100000,25,250); CHECK(sp_add(&state,&w,1,1));
    NEAR(state.width_m,25,1e-10); CHECK(state.interval_start_m==10 && state.bin_index==1);
    w=sample(110000,26.3,10); CHECK(sp_add(&state,&w,1,1)); NEAR(state.width_m,25,1e-10);
    /* Two successive finer-capable entries are required to shrink. */
    w=sample(1980000,45,10); CHECK(sp_add(&state,&w,1,1)); NEAR(state.width_m,25,1e-10);
    w=sample(3480000,60,10); CHECK(sp_add(&state,&w,1,1)); NEAR(state.width_m,15,1e-10);
    CHECK(state.width_m>=15-1e-10);
}
static void make_capacity_batch(int overflow)
{
    sp_options o=options(1,overflow ? 1 : 0);
    unsigned step,g;
    sp_init(&state,&o);
    for(step=0;step<=(overflow ? 30u : 8u);++step) {
        for(g=0;g<SP_GROUP_CAP;++g) {
            weather_sample w=sample((uint64_t)step*250000,(double)step,4);
            w.sensor_id=g; CHECK(sp_add(&state,&w,UINT64_MAX,1));
        }
    }
    sp_take_snapshot(&state,&batch);
}
static void test_capacity_and_serialization(void)
{
    weather_sample w;
    unsigned i; int length;
    sp_options o=options(10,0);
    sp_init(&state,&o); w=sample(0,0,0);
    for(i=0;i<SP_GROUP_CAP;++i) { w.sensor_id=i; CHECK(sp_add(&state,&w,1,1)); }
    w.sensor_id=99; CHECK(!sp_add(&state,&w,1,1)); CHECK(state.report.rejected_samples==1);
    make_capacity_batch(0);
    CHECK(batch.n==SP_BATCH_CAP && batch.report.dropped_records==0);
    for(i=0;i<128;++i) CHECK(batch.record[i].status==SP_COMPLETE);
    for(i=128;i<144;++i) CHECK(batch.record[i].status==SP_PARTIAL);
    length=sp_batch_json(&batch,json,sizeof(json)); CHECK(length>0);
    CHECK((size_t)length==strlen(json)); CHECK(strstr(json,"\"schema\":\"mavlink_spatial_v1\""));
    CHECK(strstr(json,"\"velocity_valid\":true")); CHECK(strstr(json,"\"navigation_segment\":1"));
    CHECK(strstr(json,"\"continuity_key\":\"18446744073709551615\""));
    CHECK(strstr(json,"\"variance\":null"));
    CHECK(sp_batch_json(&batch,json,1)==-1 && json[0]=='\0');
    batch.record[0].mean=NAN; CHECK(sp_batch_json(&batch,json,sizeof(json))==-1 && json[0]=='\0');
    make_capacity_batch(1);
    CHECK(batch.report.dropped_records>0 && batch.report.flags&SP_FLAG_RATE_LIMITED);
    CHECK(batch.report.effective_width_m>1);
    CHECK(state.completed_n==0 && !state.capacity_pressure);
    CHECK(sp_batch_json(&batch,json,sizeof(json))>0);
    sp_take_snapshot(&state,&batch); CHECK(batch.n==0);
}
static void test_actual_representative(void)
{
    sp_options o=options(10,0); weather_sample w; unsigned i;
    sp_init(&state,&o);
    for(i=0;i<5;++i) {
        w=sample((uint64_t)i*10000,i*.01,1); w.pos_error_m=i==3 ? .1f : 1.f;
        w.value=280+i; CHECK(sp_add(&state,&w,1,1));
    }
    sp_take_snapshot(&state,&batch);
    CHECK(batch.n==1 && batch.record[0].representative.seq==30);
    CHECK(batch.record[0].representative.value==283 && batch.record[0].mean==282);
    CHECK(batch.record[0].representative.p[0]==.03);
}
static void benchmark(void)
{
    sp_options o=options(10,1); unsigned i; clock_t begin=clock();
    sp_init(&state,&o);
    for(i=0;i<100000;++i) {
        weather_sample w=sample((uint64_t)i*10000,i*.01,1);
        if(!sp_add(&state,&w,1,1)) { CHECK(0); break; }
        if(i%1000==999) sp_take_snapshot(&state,&batch);
    }
    printf("spatial benchmark: %.3f us/sample (100000 observations, process CPU average); state=%zu batch=%zu record=%zu bytes\n",
           1e6*(double)(clock()-begin)/CLOCKS_PER_SEC/100000,sizeof(state),sizeof(batch),sizeof(sp_record));
    CHECK(state.report.spatial_samples==100000 && state.report.dropped_records==0);
}
int main(int argc,char **argv)
{
    if(argc==2 && strcmp(argv[1],"--json")==0) {
        make_capacity_batch(0);
        if(sp_batch_json(&batch,json,sizeof(json))<0 || failures) return 1;
        puts(json); return 0;
    }
    test_options(); test_uniform(1,1000); test_uniform(10,100);
    test_pair_and_revisions(); test_transactional_snapshot(); test_stationary(); test_unknown_breaks();
    test_continuity(); test_same_time_conflict(); test_disagreement(); test_undersampling();
    test_capacity_and_serialization(); test_actual_representative();
    if(argc==2 && strcmp(argv[1],"--bench")==0) benchmark();
    printf("SPATIAL_TEST %s %u checks, %u failures\n",failures ? "FAIL" : "PASS",checks,failures);
    return failures ? 1 : 0;
}
