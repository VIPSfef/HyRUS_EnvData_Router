#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "router_input.h"
#include "input_transport.h"
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
static uint64_t ri_now(void) { return GetTickCount64(); }
static void ri_pause(unsigned ms) { Sleep(ms); }
#else
#include <time.h>
#include <errno.h>
static uint64_t ri_now(void) { struct timespec t; (void)clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000+(unsigned long)t.tv_nsec/1000000; }
static void ri_pause(unsigned ms) { struct timespec t={(time_t)(ms/1000),(long)(ms%1000)*1000000}; while(nanosleep(&t,&t) && errno==EINTR) {} }
#endif
#define RI_CANDIDATES (IT_MAX_CANDIDATES+3u)
typedef struct { it_kind kind; char endpoint[2048]; } candidate;
typedef struct {
    mi_input decoder; it_handle transport; ri_report report;
    candidate candidate[RI_CANDIDATES];
    char serial[IT_MAX_CANDIDATES][IT_ENDPOINT_BYTES];
    uint8_t bytes[4096], heartbeat[32];
} receiver;
static receiver input;
size_t ri_storage_bytes(void) { return sizeof(input); }
static void add_candidate(receiver *s,unsigned *n,it_kind kind,const char *endpoint) {
    unsigned i;
    if(!endpoint[0] || *n>=RI_CANDIDATES) return;
    for(i=0;i<*n;++i) if(s->candidate[i].kind==kind && !strcmp(s->candidate[i].endpoint,endpoint)) return;
    s->candidate[*n].kind=kind;
    (void)snprintf(s->candidate[*n].endpoint,sizeof(s->candidate[*n].endpoint),"%s",endpoint);
    ++*n;
}
static unsigned candidates(receiver *s,const tb_config *c) {
    unsigned n=0; size_t i,count;
    if(strcmp(c->serial_port,"auto") && strcmp(c->serial_port,"none")) add_candidate(s,&n,IT_SERIAL,c->serial_port);
    add_candidate(s,&n,IT_WEBSOCKET,c->websocket_url);
    if(c->udp_enabled) {
        char endpoint[96];
        (void)snprintf(endpoint,sizeof(endpoint),"%s:%u",c->udp_bind,c->udp_port);
        add_candidate(s,&n,IT_UDP,endpoint);
    }
    if(c->auto_scan) {
        count=it_enumerate(s->serial,IT_MAX_CANDIDATES);
        for(i=0;i<count;++i) add_candidate(s,&n,IT_SERIAL,s->serial[i]);
    }
    return n;
}
static void publish(receiver *s,ri_status_fn status,void *context,const char *state) {
    if(state) (void)snprintf(s->report.status,sizeof(s->report.status),"%s",state);
    mi_get_report(&s->decoder,&s->report.mav);
    if(status) status(context,&s->report);
}
int ri_run(const tb_config *c,mi_emit_fn emit,ri_status_fn status,ri_stop_fn stop,void *context) {
    receiver *s=&input;
    mi_options options;
    uint64_t started=ri_now(),next_status=0,retry_at=0,opened_at=0,last_valid=0,next_heartbeat=0;
    unsigned count=0,index=0; int opened=0,locked=0,trusted=0;
    uint8_t sequence=0; size_t tx_len=0,tx_offset=0;
    memset(s,0,sizeof(*s));
    mi_options_default(&options);
    options.target_system=(uint8_t)c->target_system; options.target_component=(uint8_t)c->target_component;
    options.origin_auto=(uint8_t)c->origin_auto;
    options.origin_lat=c->origin[0]; options.origin_lon=c->origin[1]; options.origin_alt=c->origin[2];
    options.system_time_uncert_us=c->system_time_uncert_us; options.accel_bound_mps2=c->accel_bound_mps2;
    mi_init(&s->decoder,&options,emit,context);
    publish(s,status,context,"SEARCHING");
    for(;;) {
        uint64_t now=ri_now();
        if((stop && stop(context)) || (c->duration_ms && now-started>=c->duration_ms)) break;
        if(!opened) {
            candidate *current;
            if(now<retry_at) { ri_pause(25); continue; }
            if(index>=count) { count=candidates(s,c); index=0; }
            if(!count) { publish(s,status,context,"NO_CHANNEL"); retry_at=now+c->reconnect_ms; continue; }
            current=&s->candidate[index++];
            ++s->report.attempts;
            if(current->kind==IT_WEBSOCKET) cfg_safe_url(current->endpoint,s->report.source,sizeof(s->report.source));
            else if(current->kind==IT_UDP) (void)snprintf(s->report.source,sizeof(s->report.source),"udp://%.240s",current->endpoint);
            else (void)snprintf(s->report.source,sizeof(s->report.source),"%.255s",current->endpoint);
            publish(s,status,context,"CONNECTING");
            if(!it_open(&s->transport,current->kind,current->endpoint,c->serial_baud,c->connect_timeout_ms,c->io_timeout_ms)) {
                (void)snprintf(s->report.error,sizeof(s->report.error),"%.159s",it_error(&s->transport));
                publish(s,status,context,"CONNECT_FAILED");
                retry_at=ri_now()+(index>=count ? c->reconnect_ms : 25u); continue;
            }
            /* Never share timestamp/navigation state between two byte sources. */
            { mi_event reset; memset(&reset,0,sizeof(reset)); reset.kind=MI_RESET; if(emit) emit(context,&reset); }
            mi_init(&s->decoder,&options,emit,context);
            trusted=current->kind==IT_WEBSOCKET || (strcmp(c->serial_port,"auto") && !strcmp(current->endpoint,c->serial_port));
            opened=1; locked=0; opened_at=ri_now(); last_valid=opened_at;
            next_heartbeat=opened_at; tx_len=tx_offset=0;
            s->report.error[0]=0; ++s->report.connections;
            publish(s,status,context,"PROBING");
        } else {
            uint64_t before=s->decoder.report.frames;
            int n=it_read(&s->transport,s->bytes,sizeof(s->bytes));
            now=ri_now();
            if(n>0) mi_feed(&s->decoder,s->bytes,(size_t)n,now);
            mi_tick(&s->decoder,now);
            if(s->decoder.report.frames>before) { locked=1; last_valid=now; }
            if(n<0 || (!locked && now-opened_at>=c->probe_ms) || (locked && now-last_valid>=c->link_timeout_ms)) {
                (void)snprintf(s->report.error,sizeof(s->report.error),"%.159s",n<0 ? it_error(&s->transport) : locked ? "MAVLink timeout" : "No valid MAVLink frame on channel");
                mi_finish(&s->decoder,now);
                it_close(&s->transport); opened=0;
                publish(s,status,context,"DISCONNECTED");
                retry_at=now+(index>=count ? c->reconnect_ms : 25u); continue;
            }
            /* UDP is an observation-only endpoint. It never announces a GCS,
             * changes stream rates, sends commands, or learns a reply target. */
            if(s->transport.kind!=IT_UDP && (locked || trusted) && now>=next_heartbeat && !tx_len) {
                tx_len=mi_heartbeat(s->heartbeat,sizeof(s->heartbeat),255,191,sequence++); tx_offset=0;
                next_heartbeat=now+1000;
            }
            if(tx_len) {
                int nout=it_write(&s->transport,s->heartbeat+tx_offset,tx_len-tx_offset);
                if(nout>0) tx_offset+=(size_t)nout;
                if(nout<0 || tx_offset==tx_len) tx_len=tx_offset=0;
            }
            if(now>=next_status) { publish(s,status,context,locked ? (s->decoder.report.selected_system ? "LOCKED" : "WAIT_HEARTBEAT") : "PROBING"); next_status=now+250; }
        }
    }
    if(opened) { mi_finish(&s->decoder,ri_now()); it_close(&s->transport); }
    publish(s,status,context,"STOPPED");
    return 1;
}
