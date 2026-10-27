#include "status_report.h"

#include <float.h>
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct { char *out; size_t used,capacity; int failed; } sr_writer;

static void put(sr_writer *w,const char *format,...)
{
    va_list args;
    int length;
    if(w->failed) return;
    va_start(args,format);
    length=vsnprintf(w->out+w->used,w->capacity-w->used,format,args);
    va_end(args);
    if(length<0 || (size_t)length>=w->capacity-w->used) w->failed=1;
    else w->used+=(size_t)length;
}
static void quote(sr_writer *w,const char *text)
{
    const unsigned char *p=(const unsigned char *)text;
    put(w,"\"");
    for(;*p;++p) {
        if(*p<32 || *p=='"' || *p=='\\') put(w,"\\u%04x",(unsigned)*p);
        else put(w,"%c",*p);
    }
    put(w,"\"");
}
const char *sr_receipt_state(int known,uint64_t received,uint64_t now,
                             unsigned timeout,const char *expired)
{
    if(!known) return "WAIT";
    return now>=received && now-received<timeout ? "LIVE" : expired;
}
const char *sr_source_state(const tm_snapshot *s,const tb_config *c)
{
    const char *state=s->source_status[0] ? s->source_status : "WAITING";
    if(s->failed) return "FAILED";
    if(s->eof || s->stop) return "STOPPED";
    if(!strcmp(state,"LOCKED")) {
        const char *heartbeat=sr_receipt_state(s->have_heartbeat,s->heartbeat_received_ms,s->now_ms,c->link_timeout_ms,"LOST");
        if(!strcmp(heartbeat,"WAIT")) return "WAIT_HEARTBEAT";
        if(!strcmp(heartbeat,"LOST")) return "HEARTBEAT_LOST";
    }
    return state;
}
static void age(sr_writer *w,int known,uint64_t received,uint64_t now)
{
    if(known && now>=received) put(w,"\"%" PRIu64 "\"",now-received);
    else put(w,"null");
}
static void receipts(sr_writer *w,int have_heartbeat,uint64_t heartbeat,
                     int have_data,uint64_t data,uint64_t now,unsigned timeout)
{
    put(w,"\"heartbeat\":\"%s\",\"hb_age_ms\":",
        sr_receipt_state(have_heartbeat,heartbeat,now,timeout,"LOST"));
    age(w,have_heartbeat,heartbeat,now);
    put(w,",\"data\":\"%s\",\"data_age_ms\":",
        sr_receipt_state(have_data,data,now,timeout,"STALE"));
    age(w,have_data,data,now);
}
static void metric(sr_writer *w,const tm_snapshot *s,const tb_config *c,
                   const char *name,const char *unit,double maximum)
{
    const tm_key *key=NULL;
    unsigned i;
    int known;
    for(i=0;i<s->key_n && i<TB_MAX_KEYS;++i)
        if(!strcmp(s->keys[i].key,name) && !strcmp(s->keys[i].unit,unit)) { key=&s->keys[i]; break; }
    known=key && key->have_value && key->observations && isfinite(key->value) &&
        key->value>=0 && key->value<=maximum && s->now_ms>=key->last_receipt_ms;
    put(w,"{\"value\":");
    if(known) put(w,"%.17g",key->value); else put(w,"null");
    put(w,",\"unit\":\"%s\",\"state\":\"%s\",\"rx_age_ms\":",unit,
        !known ? "UNKNOWN" : !strcmp(sr_receipt_state(1,key->last_receipt_ms,s->now_ms,c->idle_warn_ms,"STALE"),"LIVE") ? "FRESH" : "STALE");
    age(w,known,known ? key->last_receipt_ms : 0,s->now_ms);
    put(w,",\"observations\":\"%" PRIu64 "\"}",known ? key->observations : UINT64_C(0));
}
const char *sr_position_source(const tm_snapshot *s)
{
    return s->position_kind==TM_POSITION_GPS ? "GPS" :
        s->position_kind==TM_POSITION_ESTGPS ? "ESTGPS" : "UNKNOWN";
}
int sr_position_known(const tm_snapshot *s)
{
    return s->have_last_geodetic &&
        (s->position_kind==TM_POSITION_GPS || s->position_kind==TM_POSITION_ESTGPS) &&
        isfinite(s->latitude_deg) && isfinite(s->longitude_deg) && isfinite(s->altitude_msl_m) &&
        s->latitude_deg>=-90 && s->latitude_deg<=90 &&
        s->longitude_deg>=-180 && s->longitude_deg<=180 && s->now_ms>=s->navigation_received_ms;
}
const char *sr_position_state(const tm_snapshot *s,const tb_config *c)
{
    if(!sr_position_known(s)) return "UNKNOWN";
    return s->navigation_invalid ? "INVALID" : sr_receipt_state(1,s->navigation_received_ms,s->now_ms,c->link_timeout_ms,"STALE");
}
static void position(sr_writer *w,const tm_snapshot *s,const tb_config *c)
{
    if(!sr_position_known(s)) { put(w,"null"); return; }
    put(w,"{\"source\":\"%s\",\"state\":\"%s\",\"fix\":%u,\"lat_deg\":%.17g,\"lon_deg\":%.17g,\"alt_msl_m\":%.17g,\"rx_age_ms\":",
        sr_position_source(s),sr_position_state(s,c),
        s->gps_fix,s->latitude_deg,s->longitude_deg,s->altitude_msl_m);
    age(w,1,s->navigation_received_ms,s->now_ms);
    put(w,",\"stale_after_ms\":%u}",c->link_timeout_ms);
}
int sr_encode(const tm_snapshot *s,const tb_config *c,const char *stream,
              const char *session,uint64_t generation,uint64_t sequence,
              char *output,size_t capacity)
{
    sr_writer w={output,0,capacity,0};
    const char *state;
    char endpoint[256];
    unsigned i,n;
    if(output && capacity) output[0]='\0';
    if(!output || !capacity || !s || !c || !stream || !session) return 0;
    state=sr_source_state(s,c);
    if(strstr(s->active_source,"://") && strncmp(s->active_source,"udp://",6))
        cfg_safe_url(s->active_source,endpoint,sizeof(endpoint));
    else (void)snprintf(endpoint,sizeof(endpoint),"%s",s->active_source[0] ? s->active_source : "none");
    put(&w,"{\"schema\":\"mavlink_router_status_v1\",\"stream\":"); quote(&w,stream);
    put(&w,",\"records\":[{\"seq\":\"%" PRIu64 "\",\"generation\":\"%" PRIu64 "\",\"session\":",sequence,generation);
    quote(&w,session);
    put(&w,",\"reported_monotonic_ms\":\"%" PRIu64 "\",\"run_elapsed_ms\":\"%" PRIu64 "\",\"sample_count\":0,",
        s->now_ms,s->now_ms>=s->started_ms ? s->now_ms-s->started_ms : UINT64_C(0));
    put(&w,"\"source\":{\"sysid\":%u,\"compid\":%u,\"endpoint\":",s->selected_system,s->selected_component);
    quote(&w,endpoint); put(&w,",\"state\":"); quote(&w,state);
    put(&w,",\"serial_baud\":%u,\"connected\":%s,",s->serial_baud,!strcmp(state,"LOCKED") ? "true" : "false");
    receipts(&w,s->have_heartbeat,s->heartbeat_received_ms,s->have_data,s->data_received_ms,s->now_ms,c->link_timeout_ms);
    put(&w,"},\"weather\":{\"temperature\":");
    metric(&w,s,c,"hyg_temperature","K",DBL_MAX);
    put(&w,",\"humidity\":"); metric(&w,s,c,"hyg_humidity","1",1.0);
    put(&w,"},\"position\":"); position(&w,s,c);
    put(&w,",\"packets\":{\"received\":\"%" PRIu64 "\",\"estimated_lost\":\"%" PRIu64 "\",\"duplicates\":\"%" PRIu64 "\"},\"devices\":[",
        s->received_packets,s->estimated_sequence_gaps,s->duplicate_packets);
    n=s->device_n<TM_MAX_DEVICES ? s->device_n : TM_MAX_DEVICES;
    for(i=0;i<n;++i) {
        const tm_device *d=&s->devices[i];
        put(&w,"%s{\"sysid\":%u,\"compid\":%u,",i ? "," : "",(unsigned)d->sysid,(unsigned)d->compid);
        receipts(&w,d->have_heartbeat,d->last_heartbeat_ms,d->have_data,d->last_data_ms,s->now_ms,c->link_timeout_ms);
        put(&w,",\"rx_age_ms\":"); age(&w,1,d->last_seen_ms,s->now_ms); put(&w,"}");
    }
    put(&w,"]}]}\n");
    if(w.failed) { output[0]='\0'; return 0; }
    return (int)w.used;
}
