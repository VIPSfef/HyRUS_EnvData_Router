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
#define RI_CANDIDATES (IT_MAX_CANDIDATES+2u)
#define RI_LANES 2u
#define RI_POLL_MS 20u
typedef struct { it_kind kind; char endpoint[2048]; } candidate;
typedef struct receiver receiver;
typedef struct {
    mi_input decoder;
    it_handle transport;
    receiver *owner;
    unsigned slot;
    int opened, trusted;
    uint64_t opened_at, last_valid, retry_at, next_heartbeat;
    uint8_t sequence, heartbeat[32];
    size_t tx_len, tx_offset;
    char source[256];
} lane;
struct receiver {
    lane lane[RI_LANES]; /* independent parser, boot clock and navigation */
    int active;
    unsigned view;
    mi_options options;
    mi_emit_fn emit;
    void *context;
    ri_report report;
    candidate candidate[RI_CANDIDATES];
    char serial[IT_MAX_CANDIDATES][IT_ENDPOINT_BYTES];
    uint8_t bytes[4096];
};
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
    if(c->auto_scan) {
        count=it_enumerate(s->serial,IT_MAX_CANDIDATES);
        for(i=0;i<count;++i) add_candidate(s,&n,IT_SERIAL,s->serial[i]);
    }
    return n;
}
/* The main input thread is the only caller: no callback races or source mixing.
 * Selection occurs before the first event is delivered, including an origin,
 * clock anchor or sample in the very first input chunk. Heartbeat-only packets
 * select after mi_feed returns. All bytes already decoded stay in this lane. */
static void choose(lane *l) {
    receiver *s=l->owner;
    if(s->active>=0) return;
    s->active=(int)l->slot;
    s->view=l->slot;
    ++s->report.connections;
    s->report.error[0]=0;
    if(s->emit) {
        mi_event reset;
        memset(&reset,0,sizeof(reset)); reset.kind=MI_RESET;
        s->emit(s->context,&reset);
    }
}
static void forward_event(void *context,const mi_event *event) {
    lane *l=context;
    receiver *s=l->owner;
    if(s->active<0 && l->decoder.report.frames) choose(l);
    if(s->active==(int)l->slot && s->emit) s->emit(s->context,event);
}
static void reset_lane(lane *l) {
    mi_init(&l->decoder,&l->owner->options,forward_event,l);
    l->tx_len=l->tx_offset=0;
}
static void publish(receiver *s,ri_status_fn status,void *context,const char *state) {
    lane *l=&s->lane[s->active>=0 ? (unsigned)s->active : s->view];
    if(state) (void)snprintf(s->report.status,sizeof(s->report.status),"%s",state);
    (void)snprintf(s->report.source,sizeof(s->report.source),"%s",l->source);
    mi_get_report(&l->decoder,&s->report.mav);
    if(status) status(context,&s->report);
}
static int open_lane(receiver *s,lane *l,it_kind kind,const char *endpoint,const tb_config *c,
                     ri_status_fn status,void *context) {
    unsigned timeout=c->io_timeout_ms<RI_POLL_MS ? c->io_timeout_ms : RI_POLL_MS;
    s->view=l->slot;
    if(kind==IT_WEBSOCKET) cfg_safe_url(endpoint,l->source,sizeof(l->source));
    else if(kind==IT_UDP) (void)snprintf(l->source,sizeof(l->source),"udp://%.240s",endpoint);
    else (void)snprintf(l->source,sizeof(l->source),"%.255s",endpoint);
    ++s->report.attempts;
    publish(s,status,context,"CONNECTING");
    if(!it_open(&l->transport,kind,endpoint,c->serial_baud,c->connect_timeout_ms,timeout)) {
        (void)snprintf(s->report.error,sizeof(s->report.error),"%.159s",it_error(&l->transport));
        publish(s,status,context,"CONNECT_FAILED");
        return 0;
    }
    reset_lane(l);
    l->trusted=kind==IT_WEBSOCKET || (strcmp(c->serial_port,"auto") && !strcmp(endpoint,c->serial_port));
    l->opened=1;
    l->opened_at=l->last_valid=l->next_heartbeat=ri_now();
    s->report.error[0]=0;
    publish(s,status,context,"PROBING");
    return 1;
}
static int identified(const mi_report *r) {
    unsigned i;
    for(i=0;i<r->devices;++i) {
        const mi_device *d=&r->device[i];
        if(d->have_heartbeat && d->sysid==r->selected_system && d->compid==r->selected_component) return 1;
    }
    return 0;
}
static void close_lane(lane *l) {
    if(l->opened) it_close(&l->transport);
    l->opened=0;
    l->tx_len=l->tx_offset=0;
}
int ri_run(const tb_config *c,mi_emit_fn emit,ri_status_fn status,ri_stop_fn stop,void *context) {
    receiver *s=&input;
    uint64_t started=ri_now(),next_status=0;
    unsigned count=0,index=0,i;
    memset(s,0,sizeof(*s));
    s->active=-1; s->emit=emit; s->context=context;
    mi_options_default(&s->options);
    s->options.target_system=(uint8_t)c->target_system; s->options.target_component=(uint8_t)c->target_component;
    s->options.origin_auto=(uint8_t)c->origin_auto;
    s->options.origin_lat=c->origin[0]; s->options.origin_lon=c->origin[1]; s->options.origin_alt=c->origin[2];
    s->options.system_time_uncert_us=c->system_time_uncert_us; s->options.accel_bound_mps2=c->accel_bound_mps2;
    for(i=0;i<RI_LANES;++i) { s->lane[i].owner=s; s->lane[i].slot=i; reset_lane(&s->lane[i]); }
    publish(s,status,context,"SEARCHING");
    for(;;) {
        uint64_t now=ri_now();
        int worked=0;
        if((stop && stop(context)) || (c->duration_ms && now-started>=c->duration_ms)) break;
        /* UDP remains open while serial/WebSocket candidates are probed. A
         * silent serial device cannot postpone the UDP probe by probe_ms. */
        if(s->active<0 && c->udp_enabled && !s->lane[0].opened && now>=s->lane[0].retry_at) {
            char endpoint[96];
            (void)snprintf(endpoint,sizeof(endpoint),"%s:%u",c->udp_bind,c->udp_port);
            if(!open_lane(s,&s->lane[0],IT_UDP,endpoint,c,status,context))
                s->lane[0].retry_at=ri_now()+c->reconnect_ms;
        }
        if(s->active<0 && !s->lane[1].opened && now>=s->lane[1].retry_at) {
            lane *l=&s->lane[1];
            if(index>=count) { count=candidates(s,c); index=0; }
            if(count) {
                candidate *current=&s->candidate[index++];
                if(!open_lane(s,l,current->kind,current->endpoint,c,status,context))
                    l->retry_at=ri_now()+(index>=count ? c->reconnect_ms : 25u);
            } else {
                l->retry_at=ri_now()+c->reconnect_ms;
                if(!s->lane[0].opened && !s->report.error[0]) publish(s,status,context,"NO_CHANNEL");
            }
        }
        for(i=0;i<RI_LANES;++i) {
            lane *l=&s->lane[i];
            uint64_t before;
            int n,locked;
            if(!l->opened || (s->active>=0 && s->active!=(int)i)) continue;
            worked=1;
            before=l->decoder.report.frames;
            n=it_read(&l->transport,s->bytes,sizeof(s->bytes));
            now=ri_now();
            if(n>0) {
                s->report.total_bytes+=(unsigned)n;
                mi_feed(&l->decoder,s->bytes,(size_t)n,now);
                s->report.total_frames+=l->decoder.report.frames-before;
            }
            if(l->decoder.report.frames>before) {
                if(s->active<0) choose(l);
                l->last_valid=now;
            }
            locked=s->active==(int)i;
            if(locked) {
                /* The losing source is closed, not decoded or forwarded. */
                close_lane(&s->lane[1u-i]);
                mi_tick(&l->decoder,now);
            }
            if(!locked && i==0 && n>=0 && now-l->opened_at>=c->probe_ms) {
                /* Keep the UDP port bound while releasing a peer that supplied
                 * only noise. Its partial packet must never join the next peer. */
                it_udp_probe_reset(&l->transport);
                reset_lane(l);
                l->opened_at=now;
            }
            if(n<0 || (!locked && i!=0 && now-l->opened_at>=c->probe_ms) ||
               (locked && now-l->last_valid>=c->link_timeout_ms)) {
                s->view=i;
                (void)snprintf(s->report.error,sizeof(s->report.error),"%.159s",n<0 ? it_error(&l->transport) :
                               locked ? "MAVLink timeout" : "No valid MAVLink frame on channel");
                if(locked) mi_finish(&l->decoder,now);
                close_lane(l);
                publish(s,status,context,"DISCONNECTED");
                if(locked) {
                    s->active=-1;
                    /* Start a new, isolated discovery epoch after link loss. */
                    count=index=0;
                    for(unsigned j=0;j<RI_LANES;++j) {
                        close_lane(&s->lane[j]); reset_lane(&s->lane[j]);
                        s->lane[j].retry_at=now;
                    }
                } else l->retry_at=now+(i==0 || index>=count ? c->reconnect_ms : 25u);
                continue;
            }
            /* UDP never announces a GCS, changes streams, or sends commands. */
            if(l->transport.kind!=IT_UDP && (locked || l->trusted) && now>=l->next_heartbeat && !l->tx_len) {
                l->tx_len=mi_heartbeat(l->heartbeat,sizeof(l->heartbeat),255,191,l->sequence++);
                l->tx_offset=0; l->next_heartbeat=now+1000;
            }
            if(l->tx_len) {
                int nout=it_write(&l->transport,l->heartbeat+l->tx_offset,l->tx_len-l->tx_offset);
                if(nout>0) l->tx_offset+=(size_t)nout;
                if(nout<0 || l->tx_offset==l->tx_len) l->tx_len=l->tx_offset=0;
            }
        }
        now=ri_now();
        if(now>=next_status) {
            const char *state;
            if(s->active>=0) state=identified(&s->lane[s->active].decoder.report) ? "LOCKED" : "WAIT_HEARTBEAT";
            else state=s->lane[0].opened || s->lane[1].opened ? "PROBING" : NULL;
            publish(s,status,context,state);
            next_status=now+250;
        }
        if(!worked) ri_pause(20);
    }
    if(s->active>=0) mi_finish(&s->lane[s->active].decoder,ri_now());
    for(i=0;i<RI_LANES;++i) close_lane(&s->lane[i]);
    publish(s,status,context,"STOPPED");
    return 1;
}
