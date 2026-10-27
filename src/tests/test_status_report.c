#include "status_report.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks,failures;
#define CHECK(condition) do { ++checks; if(!(condition)) { \
    fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#condition); ++failures; } } while(0)

static tm_snapshot snapshot;
static tb_config config;
static char wire[SR_WIRE_CAP];

static void fixture(void)
{
    memset(&snapshot,0,sizeof(snapshot));
    cfg_defaults(&config);
    snapshot.now_ms=123456;
    snapshot.started_ms=100000;
    snapshot.selected_system=1;
    snapshot.selected_component=1;
    snapshot.serial_baud=57600;
    (void)snprintf(snapshot.source_status,sizeof(snapshot.source_status),"LOCKED");
    (void)snprintf(snapshot.active_source,sizeof(snapshot.active_source),"COM4");
}
static const char *value(const char *object,const char *key)
{
    char field[64];
    const char *found;
    if(!object) return NULL;
    (void)snprintf(field,sizeof(field),"\"%s\":",key);
    found=strstr(object,field);
    return found ? found+strlen(field) : NULL;
}
static int literal(const char *object,const char *key,const char *expected)
{
    const char *found=value(object,key);
    size_t n=strlen(expected);
    return found && !strncmp(found,expected,n) && strchr(",}]",found[n])!=NULL;
}
static unsigned hex(unsigned char c)
{
    if(c>='0' && c<='9') return (unsigned)(c-'0');
    if(c>='a' && c<='f') return (unsigned)(c-'a')+10u;
    if(c>='A' && c<='F') return (unsigned)(c-'A')+10u;
    return 16;
}
/* Read the serializer's JSON string as bytes, so escaping tests compare the
 * recovered value rather than requiring one particular spelling of escapes. */
static int string_value(const char *object,const char *key,char *out,size_t capacity)
{
    const unsigned char *p=(const unsigned char *)value(object,key);
    size_t n=0;
    if(!p || *p++!='"' || !capacity) return 0;
    while(*p && *p!='"') {
        unsigned c=*p++;
        if(c=='\\') {
            c=*p++;
            if(c=='u') {
                unsigned number=0;
                for(unsigned i=0;i<4;++i) {
                    unsigned digit=hex(*p++);
                    if(digit>15) return 0;
                    number=(number<<4)|digit;
                }
                if(number>255) return 0;
                c=number;
            } else if(c=='n') c='\n';
            else if(c=='r') c='\r';
            else if(c=='t') c='\t';
            else if(c=='b') c='\b';
            else if(c=='f') c='\f';
            else if(c!='"' && c!='\\' && c!='/') return 0;
        }
        if(n+1>=capacity) return 0;
        out[n++]=(char)c;
    }
    out[n]=0;
    return *p=='"';
}
static int string_is(const char *object,const char *key,const char *expected)
{
    char text[256];
    return string_value(object,key,text,sizeof(text)) && !strcmp(text,expected);
}
static int u64_is(const char *object,const char *key,uint64_t expected)
{
    char text[32];
    (void)snprintf(text,sizeof(text),"%" PRIu64,expected);
    return string_is(object,key,text);
}
static unsigned occurrences(const char *text,const char *needle)
{
    unsigned n=0;
    size_t length=strlen(needle);
    if(!text) return 0;
    while((text=strstr(text,needle))!=NULL) { ++n; text+=length; }
    return n;
}
static void escaping_and_privacy(void)
{
    static const char text[]="station\"\\\n\t\r\b\f\001\037 \xe2\x84\x83";
    static const char session[]="\"}],\"forged\":true,\"records\":[{\"";
    int length,escaped=1;
    fixture();
    (void)snprintf(snapshot.active_source,sizeof(snapshot.active_source),
                   "wss://user:secret@example.test/telemetry?token=hidden#private");
    length=sr_encode(&snapshot,&config,text,session,UINT64_MAX,UINT64_MAX,wire,sizeof(wire));
    CHECK(length>0 && (size_t)length==strlen(wire));
    CHECK(string_is(wire,"stream",text));
    CHECK(string_is(wire,"session",session));
    CHECK(u64_is(wire,"seq",UINT64_MAX));
    CHECK(u64_is(wire,"generation",UINT64_MAX));
    CHECK(!strstr(wire,"secret") && !strstr(wire,"hidden") && !strstr(wire,"private"));
    CHECK(!strstr(wire,"\"forged\":true"));
    for(int i=0;i<length-1;++i) if((unsigned char)wire[i]<32) escaped=0;
    CHECK(escaped);
}
static void bounded_output(void)
{
    char guarded[SR_WIRE_CAP+2];
    size_t capacities[7];
    int length;
    fixture();
    length=sr_encode(&snapshot,&config,"stream","session",1,1,wire,sizeof(wire));
    CHECK(length>32);
    if(length<=32) return;
    capacities[0]=0; capacities[1]=1; capacities[2]=2; capacities[3]=32;
    capacities[4]=(size_t)length-1; capacities[5]=(size_t)length; capacities[6]=(size_t)length+1;
    for(unsigned i=0;i<sizeof(capacities)/sizeof(capacities[0]);++i) {
        size_t capacity=capacities[i];
        int result;
        memset(guarded,'#',sizeof(guarded));
        result=sr_encode(&snapshot,&config,"stream","session",1,1,guarded+1,capacity);
        CHECK(guarded[0]=='#' && guarded[capacity+1]=='#');
        if(capacity<=(size_t)length) {
            CHECK(result==0);
            CHECK(capacity ? guarded[1]=='\0' : guarded[1]=='#');
        } else {
            CHECK(result==length);
            CHECK(!strcmp(guarded+1,wire));
        }
    }
    CHECK(!sr_encode(&snapshot,&config,"stream","session",1,1,NULL,8));
    guarded[0]='#';
    CHECK(!sr_encode(NULL,&config,"stream","session",1,1,guarded,sizeof(guarded)) && !guarded[0]);
}
static void source_health_and_ages(void)
{
    const char *source,*weather,*temperature,*humidity;
    fixture();
    snapshot.have_heartbeat=1; snapshot.heartbeat_received_ms=snapshot.now_ms-41;
    snapshot.have_data=1; snapshot.data_received_ms=snapshot.now_ms-7;
    snapshot.key_n=2;
    for(unsigned i=0;i<snapshot.key_n;++i) {
        tm_key *key=&snapshot.keys[i];
        (void)snprintf(key->key,sizeof(key->key),"%s",i ? "hyg_humidity" : "hyg_temperature");
        (void)snprintf(key->unit,sizeof(key->unit),"%s",i ? "1" : "K");
        key->have_value=1; key->observations=9+i;
        key->value=i ? 0.5 : 298.15;
        key->last_receipt_ms=snapshot.now_ms-(i ? config.idle_warn_ms+57u : 17u);
    }
    CHECK(sr_encode(&snapshot,&config,"stream","session",4,5,wire,sizeof(wire))>0);
    source=value(wire,"source"); weather=value(wire,"weather");
    temperature=value(weather,"temperature"); humidity=value(weather,"humidity");
    CHECK(literal(source,"connected","true") && string_is(source,"state","LOCKED"));
    CHECK(u64_is(wire,"run_elapsed_ms",23456));
    CHECK(u64_is(wire,"reported_monotonic_ms",snapshot.now_ms));
    CHECK(u64_is(source,"hb_age_ms",41) && u64_is(source,"data_age_ms",7));
    CHECK(u64_is(temperature,"rx_age_ms",17) && string_is(temperature,"state","FRESH"));
    CHECK(u64_is(humidity,"rx_age_ms",config.idle_warn_ms+57u) && string_is(humidity,"state","STALE"));
    CHECK(u64_is(temperature,"observations",9) && u64_is(humidity,"observations",10));
    CHECK(literal(wire,"sample_count","0"));

    /* A live GCS and selected-device data must not hide the vehicle heartbeat
     * expiring exactly at its timeout. The device list has independent ages. */
    snapshot.heartbeat_received_ms=snapshot.now_ms-config.link_timeout_ms;
    snapshot.device_n=1;
    snapshot.devices[0].sysid=255; snapshot.devices[0].compid=190;
    snapshot.devices[0].have_heartbeat=1;
    snapshot.devices[0].last_heartbeat_ms=snapshot.now_ms;
    snapshot.devices[0].last_seen_ms=snapshot.now_ms;
    CHECK(!strcmp(sr_source_state(&snapshot,&config),"HEARTBEAT_LOST"));
    CHECK(sr_encode(&snapshot,&config,"stream","session",4,6,wire,sizeof(wire))>0);
    source=value(wire,"source");
    CHECK(literal(source,"connected","false") && string_is(source,"heartbeat","LOST"));
    CHECK(string_is(source,"data","LIVE") && string_is(value(wire,"devices"),"heartbeat","LIVE"));
    snapshot.have_heartbeat=0;
    CHECK(!strcmp(sr_source_state(&snapshot,&config),"WAIT_HEARTBEAT"));
    snapshot.stop=1;
    CHECK(!strcmp(sr_source_state(&snapshot,&config),"STOPPED"));
    snapshot.failed=1;
    CHECK(!strcmp(sr_source_state(&snapshot,&config),"FAILED"));
    CHECK(!strcmp(sr_receipt_state(1,0,0,config.link_timeout_ms,"LOST"),"LIVE"));
    CHECK(!strcmp(sr_receipt_state(0,0,0,config.link_timeout_ms,"LOST"),"WAIT"));
}
static void unknown_weather(void)
{
    fixture();
    CHECK(sr_encode(&snapshot,&config,"stream","session",0,1,wire,sizeof(wire))>0);
    CHECK(literal(value(wire,"source"),"hb_age_ms","null"));
    for(unsigned i=0;i<2;++i) {
        const char *metric=value(value(wire,"weather"),i ? "humidity" : "temperature");
        CHECK(literal(metric,"value","null") && literal(metric,"rx_age_ms","null"));
        CHECK(string_is(metric,"state","UNKNOWN") && u64_is(metric,"observations",0));
    }
    snapshot.key_n=1;
    (void)snprintf(snapshot.keys[0].key,sizeof(snapshot.keys[0].key),"hyg_humidity");
    (void)snprintf(snapshot.keys[0].unit,sizeof(snapshot.keys[0].unit),"1");
    snapshot.keys[0].have_value=1; snapshot.keys[0].observations=12;
    snapshot.keys[0].last_receipt_ms=snapshot.now_ms;
    for(unsigned invalid=0;invalid<3;++invalid) {
        snapshot.keys[0].value=invalid==0 ? NAN : invalid==1 ? -0.1 : 1.1;
        CHECK(sr_encode(&snapshot,&config,"stream","session",0,2,wire,sizeof(wire))>0);
        CHECK(literal(value(value(wire,"weather"),"humidity"),"value","null"));
        CHECK(u64_is(value(value(wire,"weather"),"humidity"),"observations",0));
    }
}
static void maximum_devices(void)
{
    char expected[SR_WIRE_CAP];
    fixture();
    snapshot.device_n=TM_MAX_DEVICES;
    for(unsigned i=0;i<TM_MAX_DEVICES;++i) {
        tm_device *device=&snapshot.devices[i];
        device->sysid=(uint8_t)(i+1); device->compid=1;
        device->have_heartbeat=1; device->have_data=1;
        device->last_heartbeat_ms=snapshot.now_ms-3;
        device->last_data_ms=snapshot.now_ms-1;
        device->last_seen_ms=snapshot.now_ms-1;
    }
    CHECK(sr_encode(&snapshot,&config,"stream","session",UINT64_MAX,UINT64_MAX,wire,sizeof(wire))>0);
    CHECK(occurrences(value(wire,"devices"),"\"sysid\":")==TM_MAX_DEVICES);
    CHECK(occurrences(value(wire,"devices"),"\"rx_age_ms\":")==TM_MAX_DEVICES);
    memcpy(expected,wire,strlen(wire)+1);
    snapshot.device_n=TM_MAX_DEVICES+1;
    CHECK(sr_encode(&snapshot,&config,"stream","session",UINT64_MAX,UINT64_MAX,wire,sizeof(wire))>0);
    CHECK(!strcmp(expected,wire));
}
static void retained_navigation(void)
{
    const char *position;
    fixture();
    CHECK(sr_encode(&snapshot,&config,"stream","session",0,1,wire,sizeof(wire))>0);
    CHECK(literal(wire,"position","null"));
    snapshot.have_last_geodetic=1;
    snapshot.have_geodetic=0; /* Alignment expired, but the last point is known. */
    snapshot.position_kind=TM_POSITION_ESTGPS;
    snapshot.latitude_deg=37.5; snapshot.longitude_deg=127.5; snapshot.altitude_msl_m=100;
    snapshot.navigation_received_ms=snapshot.now_ms-1000;
    snapshot.gps_fix=3;
    CHECK(sr_position_known(&snapshot));
    CHECK(sr_encode(&snapshot,&config,"stream","session",0,2,wire,sizeof(wire))>0);
    position=value(wire,"position");
    CHECK(string_is(position,"source","ESTGPS") && string_is(position,"state","LIVE"));
    CHECK(literal(position,"lat_deg","37.5") && u64_is(position,"rx_age_ms",1000));
    snapshot.now_ms=snapshot.navigation_received_ms+config.link_timeout_ms;
    CHECK(!strcmp(sr_position_state(&snapshot,&config),"STALE"));
    snapshot.navigation_invalid=1;
    CHECK(!strcmp(sr_position_state(&snapshot,&config),"INVALID"));
    snapshot.latitude_deg=NAN;
    CHECK(!sr_position_known(&snapshot));
    CHECK(sr_encode(&snapshot,&config,"stream","session",0,3,wire,sizeof(wire))>0);
    CHECK(literal(wire,"position","null"));
}
int main(void)
{
    escaping_and_privacy();
    bounded_output();
    source_health_and_ages();
    unknown_weather();
    maximum_devices();
    retained_navigation();
    printf("status report: %u checks, %u failures\n",checks,failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
