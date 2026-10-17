/* Independent numerical, ordering, alignment and serialization checks. */
#include "telemetry.h"
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

static unsigned checks;
#define CHECK(c) do { ++checks; if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 0; \
} } while (0)

static int close_enough(double x, double y, double tol)
{ return isfinite(x) && fabs(x-y) <= tol; }

static tb_sample sample(uint64_t t, uint32_t seq, const char *key, const char *unit, double value)
{
    tb_sample s;
    memset(&s, 0, sizeof(s));
    s.t_us=t; s.seq=seq; s.quality=5;
    (void)snprintf(s.key,sizeof(s.key),"%s",key);
    (void)snprintf(s.unit,sizeof(s.unit),"%s",unit);
    s.value=value;
    s.p[0]=123456789.12345679; s.p[1]=-987654321.01234567; s.p[2]=120.125;
    s.v[0]=35.5f; s.v[1]=-2.25f; s.v[2]=0.125f;
    s.time_uncert_us=123; s.nav_age_us=1234; s.pos_error_m=0.25f;
    s.utc_us=UINT64_C(1800000000000000)+t%1000000;
    s.utc_uncert_us=4567;
    return s;
}

static const tb_stat *find_stat(const tb_batch *b, const char *key)
{
    uint32_t i;
    for(i=0;i<b->key_n;++i) if(!strcmp(b->stat[i].key,key)) return &b->stat[i];
    return NULL;
}

static int rejected_unchanged(tb_batch *b,const tb_sample *s)
{
    tb_batch before=*b;
    return tb_batch_add(b,s)==0 && memcmp(&before,b,sizeof(before))==0;
}

static int check_statistics(void)
{
    tb_batch b;
    const tb_stat *st;
    unsigned i;
    tb_batch_init(&b,1000000);
    for(i=0;i<4;++i) {
        tb_sample s=sample(1000000+i*100000,i+1,"custom","custom_unit",i+1);
        CHECK(tb_batch_add(&b,&s)==1);
        s=sample(s.t_us,i+1,"constant","one",42); CHECK(tb_batch_add(&b,&s)==1);
        s=sample(s.t_us,i+1,"missing","one",NAN); CHECK(tb_batch_add(&b,&s)==1);
        s=sample(s.t_us,i+1,"large","one",100000000.0+i*0.125); CHECK(tb_batch_add(&b,&s)==1);
    }
    CHECK(b.n==16 && b.raw_n==16 && b.raw_dropped==0 && b.key_n==4);
    st=find_stat(&b,"custom");
    CHECK(st && st->n==4 && st->mean==2.5 && st->m2==5 && st->min==1 && st->max==4);
    st=find_stat(&b,"constant");
    CHECK(st && st->n==4 && st->mean==42 && st->m2==0 && st->min==42 && st->max==42);
    st=find_stat(&b,"missing"); CHECK(st && st->n==0);
    st=find_stat(&b,"large");
    CHECK(st && st->n==4 && st->mean==100000000.1875 && st->m2==0.078125);
    {
        tb_sample s=sample(1000000,1,"new_sensor","count",-123.5);
        CHECK(tb_batch_add(&b,&s)==1); CHECK(b.key_n==5);
        st=find_stat(&b,"custom"); CHECK(st && st->mean==2.5 && st->m2==5);
        st=find_stat(&b,"new_sensor"); CHECK(st && st->n==1 && st->mean==-123.5 && st->m2==0);
    }
    return 1;
}

static int check_canonical_order(void)
{
    tb_batch a,b;
    char out_a[65536],out_b[65536];
    const char *keys[]={"zeta","alpha","middle","extra"};
    unsigned i,k;
    tb_batch_init(&a,0); tb_batch_init(&b,0);
    for(i=0;i<10;++i) for(k=0;k<4;++k) {
        tb_sample s=sample(i*1000,i,keys[k],"one",0.1*i+k);
        CHECK(tb_batch_add(&a,&s)==1);
    }
    for(k=4;k-- > 0;) for(i=0;i<10;++i) {
        tb_sample s=sample(i*1000,i,keys[k],"one",0.1*i+k);
        CHECK(tb_batch_add(&b,&s)==1);
    }
    CHECK(tb_batch_json(&a,out_a,sizeof(out_a))<0);
    tb_batch_prepare(&a); tb_batch_prepare(&b);
    CHECK(tb_batch_json(&a,out_a,sizeof(out_a))>0);
    CHECK(tb_batch_json(&b,out_b,sizeof(out_b))>0);
    CHECK(strcmp(out_a,out_b)==0);
    tb_batch_prepare(&a);
    CHECK(tb_batch_json(&a,out_a,sizeof(out_a))>0 && strcmp(out_a,out_b)==0);
    {
        tb_sample s=sample(10000,10,"alpha","one",17);
        CHECK(tb_batch_add(&a,&s)==1);
        CHECK(tb_batch_json(&a,out_a,sizeof(out_a))<0);
        tb_batch_prepare(&a); CHECK(tb_batch_json(&a,out_a,sizeof(out_a))>0);
    }
    return 1;
}

static int check_uncertainty_statistics(void)
{
    tb_batch b;
    const tb_stat *st;
    unsigned i;
    tb_batch_init(&b,0);
    for(i=0;i<3;++i) {
        tb_sample s=sample(i,i,"uncertain","u",i==1 ? NAN : i+1);
        s.v[0]=i==2 ? 20 : 10; s.v[1]=s.v[2]=0;
        s.time_uncert_us=i==1 ? 20000 : (i+1)*1000;
        s.pos_error_m=i==1 ? 999 : (float)(i+1);
        CHECK(tb_batch_add(&b,&s)==1);
    }
    st=find_stat(&b,"uncertain");
    CHECK(st && st->n==2 && st->mean==2);
    CHECK(st->mean_time_uncert_us==2000 && st->max_time_uncert_us==3000);
    CHECK(close_enough(st->mean_position_error_m,2.035,1e-12));
    CHECK(close_enough(st->max_position_error_m,3.06,1e-12));
    return 1;
}

static int check_rejection(void)
{
    tb_batch b;
    tb_sample s;
    unsigned k;
    tb_batch_init(&b,1000000);
    s=sample(999999,1,"sensor","one",10); CHECK(rejected_unchanged(&b,&s));
    s.t_us=2000000; CHECK(rejected_unchanged(&b,&s));
    s.t_us=1000000; CHECK(tb_batch_add(&b,&s)==1);
    s.seq=2; CHECK(rejected_unchanged(&b,&s));
    s.t_us=1000001; s.seq=1; CHECK(rejected_unchanged(&b,&s));
    s.seq=0; CHECK(rejected_unchanged(&b,&s)); s.seq=2;
    for(k=0;k<3;++k) {
        tb_sample bad=s;
        bad.p[k]=INFINITY; CHECK(rejected_unchanged(&b,&bad));
        bad=s; bad.p[k]=NAN; CHECK(rejected_unchanged(&b,&bad));
        bad=s; bad.p[k]=1e9+1.0; CHECK(rejected_unchanged(&b,&bad));
        bad=s; bad.v[k]=INFINITY; CHECK(rejected_unchanged(&b,&bad));
        bad=s; bad.v[k]=NAN; CHECK(rejected_unchanged(&b,&bad));
        bad=s; bad.v[k]=2e9f; CHECK(rejected_unchanged(&b,&bad));
    }
    {
        tb_sample bad=s;
        bad.value=INFINITY; CHECK(rejected_unchanged(&b,&bad));
        bad.value=-INFINITY; CHECK(rejected_unchanged(&b,&bad));
        bad=s; bad.value=2e9; CHECK(rejected_unchanged(&b,&bad));
        bad=s; bad.time_uncert_us=20001; CHECK(rejected_unchanged(&b,&bad));
        bad=s; bad.nav_age_us=200001; CHECK(rejected_unchanged(&b,&bad));
        bad=s; bad.pos_error_m=-1; CHECK(rejected_unchanged(&b,&bad));
        bad=s; bad.pos_error_m=NAN; CHECK(rejected_unchanged(&b,&bad));
        bad=s; bad.pos_error_m=INFINITY; CHECK(rejected_unchanged(&b,&bad));
        bad=s; bad.key[0]='\0'; CHECK(rejected_unchanged(&b,&bad));
        bad=s; memset(bad.key,'x',sizeof(bad.key)); CHECK(rejected_unchanged(&b,&bad));
        bad=s; bad.key[0]='"'; CHECK(rejected_unchanged(&b,&bad));
        bad=s; memset(bad.unit,'x',sizeof(bad.unit)); CHECK(rejected_unchanged(&b,&bad));
        bad=s; (void)snprintf(bad.unit,sizeof(bad.unit),"other"); CHECK(rejected_unchanged(&b,&bad));
    }
    tb_batch_prepare(&b); s.seq=1; CHECK(rejected_unchanged(&b,&s));
    s.seq=2; s.time_uncert_us=20000; s.nav_age_us=200000;
    CHECK(tb_batch_add(&b,&s)==1);
    s.t_us=1000000; s.seq=3; CHECK(rejected_unchanged(&b,&s));
    return 1;
}

static int check_keys_and_time_edges(void)
{
    tb_batch b;
    tb_sample s;
    unsigned i;
    tb_batch_init(&b,0);
    for(i=0;i<TB_MAX_KEYS;++i) {
        char key[TB_KEY_BYTES];
        (void)snprintf(key,sizeof(key),"key%02u",i);
        s=sample(0,1,key,"unit",i); CHECK(tb_batch_add(&b,&s)==1);
    }
    s=sample(1,2,"overflow_key","unit",10); CHECK(rejected_unchanged(&b,&s));
    CHECK(b.key_n==TB_MAX_KEYS && b.n==TB_MAX_KEYS);
    s=sample(1,2,"key00","unit",7); CHECK(tb_batch_add(&b,&s)==1);
    tb_batch_init(&b,UINT64_C(1800000000000000));
    s=sample(UINT64_C(1800000000000100),UINT32_MAX,"s","u",12); CHECK(tb_batch_add(&b,&s)==1);
    s.t_us++; s.seq=0; CHECK(rejected_unchanged(&b,&s));
    tb_batch_init(&b,0); s=sample(0,0,"s","u",0); CHECK(tb_batch_add(&b,&s)==1);
    s.t_us=TB_WINDOW_US-1; s.seq=1; CHECK(tb_batch_add(&b,&s)==1);
    s.t_us=TB_WINDOW_US; s.seq=2; CHECK(rejected_unchanged(&b,&s));
    tb_batch_init(&b,UINT64_MAX/TB_WINDOW_US*TB_WINDOW_US);
    s=sample(UINT64_MAX,1,"s","u",10); CHECK(tb_batch_add(&b,&s)==1);
    return 1;
}

static int check_capacity_statistics(void)
{
    tb_batch b;
    unsigned i;
    double total=0;
    const unsigned count=TB_CAPACITY+37;
    tb_batch_init(&b,0);
    for(i=0;i<count;++i) {
        tb_sample s=sample(i+1,i,"s","u",i%100);
        CHECK(tb_batch_add(&b,&s)==1); total+=i%100;
    }
    CHECK(b.n==count && b.raw_n==TB_CAPACITY && b.raw_dropped==37);
    CHECK(b.stat[0].n==count && close_enough(b.stat[0].mean,total/count,1e-11));
    CHECK(b.raw[0].seq==0 && b.raw[TB_CAPACITY-1].seq==TB_CAPACITY-1);
    return 1;
}

static int align_rejected_unchanged(const tb_nav *a,const tb_nav *b,uint64_t t,double accel,tb_sample *out)
{
    tb_sample before=*out;
    return tb_align(a,b,t,accel,out)==0 && memcmp(&before,out,sizeof(before))==0;
}

static int check_alignment(void)
{
    const uint64_t start=UINT64_C(1800000000000000);
    tb_nav a,b,bad;
    tb_sample out=sample(start+50000,77,"arbitrary","my_unit",123.123456789);
    uint64_t original_utc=out.utc_us;
    unsigned k;
    memset(&a,0,sizeof(a)); memset(&b,0,sizeof(b));
    a.t_us=start; b.t_us=start+200000;
    a.p[0]=10; b.p[0]=18; a.v[0]=b.v[0]=40;
    a.p[1]=-4; b.p[1]=-6; a.v[1]=b.v[1]=-10;
    a.p[2]=120; b.p[2]=120.2; a.v[2]=b.v[2]=1;
    a.pos_error_m=1; b.pos_error_m=3;
    CHECK(tb_align(&a,&b,start+50000,2,&out)==1);
    CHECK(out.t_us==start+50000 && out.seq==77 && out.quality==5);
    CHECK(!strcmp(out.key,"arbitrary") && !strcmp(out.unit,"my_unit"));
    CHECK(out.value==123.123456789 && out.time_uncert_us==123 && out.utc_us==original_utc);
    CHECK(out.utc_uncert_us==4567);
    CHECK(close_enough(out.p[0],12,1e-12) && close_enough(out.p[1],-4.5,1e-12) && close_enough(out.p[2],120.05,1e-11));
    CHECK(out.v[0]==40 && out.v[1]==-10 && out.v[2]==1);
    CHECK(out.nav_age_us==150000);
    CHECK(close_enough(out.pos_error_m,1.51,2e-7)); /* 1.5 + 2*0.2^2/8, rounded upward */
    CHECK(out.pos_error_m>=1.51);
    CHECK(align_rejected_unchanged(&a,&b,start-1,2,&out));
    CHECK(align_rejected_unchanged(&a,&b,b.t_us+1,2,&out));
    CHECK(align_rejected_unchanged(&b,&a,start+50000,2,&out));
    bad=b; bad.t_us++; CHECK(align_rejected_unchanged(&a,&bad,start+50000,2,&out));
    CHECK(align_rejected_unchanged(&a,&b,out.t_us,-1,&out));
    CHECK(align_rejected_unchanged(&a,&b,out.t_us,NAN,&out));
    CHECK(align_rejected_unchanged(&a,&b,out.t_us,INFINITY,&out));
    CHECK(align_rejected_unchanged(&a,&b,out.t_us,1000001,&out));
    for(k=0;k<3;++k) {
        bad=a; bad.p[k]=NAN; CHECK(align_rejected_unchanged(&bad,&b,out.t_us,2,&out));
        bad=b; bad.v[k]=INFINITY; CHECK(align_rejected_unchanged(&a,&bad,out.t_us,2,&out));
    }
    bad=a; bad.pos_error_m=-1; CHECK(align_rejected_unchanged(&bad,&b,out.t_us,2,&out));
    bad=b; bad.pos_error_m=INFINITY; CHECK(align_rejected_unchanged(&a,&bad,out.t_us,2,&out));
    CHECK(align_rejected_unchanged(NULL,&b,out.t_us,2,&out));
    out.t_us=a.t_us; CHECK(tb_align(&a,&a,a.t_us,0,&out)==1);
    CHECK(out.p[0]==a.p[0] && out.nav_age_us==0 && out.pos_error_m==1);
    return 1;
}

static int check_serializer_bounds(void)
{
    tb_batch b;
    tb_sample s=sample(12,1,"s","u",10);
    char large[65536],exact[65536];
    struct { unsigned char prefix[16]; char out[32]; unsigned char suffix[16]; } guard;
    size_t cap,i;
    int n;
    tb_batch_init(&b,0); tb_batch_prepare(&b); CHECK(tb_batch_json(&b,large,sizeof(large))>0);
    CHECK(tb_batch_add(&b,&s)==1); tb_batch_prepare(&b);
    n=tb_batch_json(&b,large,sizeof(large)); CHECK(n>0 && (size_t)n==strlen(large));
    CHECK(tb_batch_json(&b,exact,(size_t)n+1)==n && strcmp(large,exact)==0);
    CHECK(tb_batch_json(&b,exact,(size_t)n)<0);
    for(cap=0;cap<=sizeof(guard.out);++cap) {
        memset(&guard,0xA5,sizeof(guard)); CHECK(tb_batch_json(&b,guard.out,cap)<0);
        for(i=0;i<sizeof(guard.prefix);++i) CHECK(guard.prefix[i]==0xA5);
        for(i=cap;i<sizeof(guard.out);++i) CHECK((unsigned char)guard.out[i]==0xA5);
        for(i=0;i<sizeof(guard.suffix);++i) CHECK(guard.suffix[i]==0xA5);
    }
    return 1;
}

static int fixture(const char *kind)
{
    tb_batch b;
    char out[262144];
    unsigned i,n;
    const uint64_t epoch=!strcmp(kind,"large_time") ?
        UINT64_MAX/TB_WINDOW_US*TB_WINDOW_US : UINT64_C(1800000000000000);
    tb_batch_init(&b,epoch);
    n=!strcmp(kind,"empty") ? 0 : !strcmp(kind,"overflow") ? TB_CAPACITY+37 :
        !strcmp(kind,"single") || !strcmp(kind,"large_time") ? 1 : 4;
    for(i=0;i<n;++i) {
        tb_sample s=sample(!strcmp(kind,"large_time") ? UINT64_MAX : epoch+i*1000,
            100+i,"custom_metric","my_unit",i==1 || !strcmp(kind,"missing") ? NAN :
            123.12345678901235+(i%100)*0.125);
        s.p[2]=120.125+i*0.1;
        if(!strcmp(kind,"large_time")) s.utc_us=UINT64_MAX;
        if(i==2) s.utc_us=0;
        if(!tb_batch_add(&b,&s)) return 0;
    }
    tb_batch_prepare(&b);
    if(tb_batch_json_mode(&b,out,sizeof(out),strcmp(kind,"summary")!=0)<0) return 0;
    puts(out); return 1;
}

static double seconds(void)
{
#ifdef _WIN32
    LARGE_INTEGER now,hz;
    QueryPerformanceFrequency(&hz); QueryPerformanceCounter(&now);
    return (double)now.QuadPart/(double)hz.QuadPart;
#else
    return (double)clock()/CLOCKS_PER_SEC;
#endif
}

static int benchmark(void)
{
    tb_batch b;
    tb_sample s=sample(0,0,"sensor0","u",10);
    uint32_t i;
    uint64_t window_us=0;
    double start,elapsed;
    volatile uint32_t accepted=0;
    start=seconds();
    for(i=0;i<1000000;++i) {
        if(i%400==0) { window_us=(uint64_t)(i/400)*TB_WINDOW_US; tb_batch_init(&b,window_us); }
        s.key[6]=(char)('0'+i%4);
        s.t_us=window_us+(i%400/4)*10000; s.seq=i; s.value=(double)(i%101)*0.5;
        accepted+=(uint32_t)tb_batch_add(&b,&s);
    }
    elapsed=seconds()-start;
    if(accepted!=1000000 || elapsed<=0 || b.n!=400 || b.raw_n!=400 || b.raw_dropped || b.key_n!=4) return 0;
    printf("{\"scenario\":\"normal_100Hz_4keys_400_observations_per_window\",\"windows\":2500,"
        "\"observations_per_window\":400,\"initialization_included\":true,\"prepare_included\":false,"
        "\"samples\":%" PRIu32 ",\"seconds\":%.9g,\"updates_per_second\":%.9g,"
        "\"ns_per_update\":%.9g,\"sizeof_sample\":%zu,\"sizeof_batch\":%zu}\n",
        accepted,elapsed,accepted/elapsed,elapsed*1e9/accepted,sizeof(tb_sample),sizeof(tb_batch));
    return 1;
}

int main(int argc,char **argv)
{
    if(argc==3 && !strcmp(argv[1],"--fixture")) return fixture(argv[2]) ? 0 : 1;
    if(argc==2 && !strcmp(argv[1],"--bench")) return benchmark() ? 0 : 1;
    if(argc!=1) return 2;
    if(!check_statistics() || !check_canonical_order() || !check_uncertainty_statistics() || !check_rejection() ||
        !check_keys_and_time_edges() || !check_capacity_statistics() ||
        !check_alignment() || !check_serializer_bounds()) return 1;
    printf("PASS %u C checks\n",checks); return 0;
}
