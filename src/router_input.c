#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "router_input.h"
#include "input_transport.h"
#include "serial_baud.h"
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
typedef enum { RI_UDP_LANE, RI_ROTATING_LANE, RI_LANE_COUNT } ri_lane_id;
typedef enum { RI_TX_HEARTBEAT, RI_TX_QGC, RI_TX_RATE } ri_tx_kind;
enum { RI_POLL_MS=20, RI_CANDIDATE_GAP_MS=25, RI_STATUS_MS=250, RI_HEARTBEAT_MS=1000 };
typedef struct { it_kind kind; char endpoint[2048]; } candidate;
typedef struct receiver receiver;
typedef struct {
    mi_input decoder;
    qb_parser wire_parser;
    mr_controller rates;
    it_handle transport;
    receiver *owner;
    ri_lane_id slot;
    unsigned serial_baud;
    int opened, trusted;
    uint64_t opened_at, last_valid, retry_at, next_heartbeat;
    uint8_t sequence, tx[MAVLINK_MAX_PACKET_LEN];
    ri_tx_kind tx_kind;
    size_t tx_len, tx_offset;
    char source[256];
} lane;
struct receiver {
    lane lane[RI_LANE_COUNT]; /* independent parser, boot clock and navigation */
    int active;
    unsigned view;
    unsigned candidate_count, candidate_next, baud_pass;
    mi_options options;
    const tb_config *config;
    qb_bridge bridge;
    uint64_t qgc_retry_at;
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
    if(c->auto_scan) {
        /* Re-enumerate each rate pass so hotplugged ports join discovery.
         * Every serial candidate gets the current rate before any fallback. */
        count=it_enumerate(s->serial,IT_MAX_CANDIDATES);
        for(i=0;i<count;++i) add_candidate(s,&n,IT_SERIAL,s->serial[i]);
    }
    if(!s->baud_pass) add_candidate(s,&n,IT_WEBSOCKET,c->websocket_url);
    return n;
}
static void ensure_qgc(receiver *s,uint64_t now) {
    if(!s->config->qgc_enabled || s->active<0 ||
       s->lane[s->active].transport.kind!=IT_SERIAL || s->bridge.report.opened ||
       now<s->qgc_retry_at) return;
    if(!qb_open(&s->bridge,s->config->qgc_host,s->config->qgc_port))
        s->qgc_retry_at=now+s->config->reconnect_ms;
}
/* One advancement point for open errors, probe timeouts and empty passes.
 * A scan cycle traverses all ports at one baud, then advances the baud. */
static void schedule_retry(receiver *s,lane *l,const tb_config *c,uint64_t now) {
    unsigned delay=RI_CANDIDATE_GAP_MS;
    if(l->slot==RI_UDP_LANE) delay=c->reconnect_ms;
    else if(s->candidate_next>=s->candidate_count) {
        s->candidate_next=s->candidate_count=0;
        if(c->auto_scan && serial_baud_probe(c->serial_baud,s->baud_pass+1)) ++s->baud_pass;
        else { s->baud_pass=0; delay=c->reconnect_ms; }
    }
    l->retry_at=now+delay;
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
    ensure_qgc(s,ri_now());
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
/* Routing uses the official wire parser, independently of sensor acceptance.
 * In particular, an opaque signed packet can be forwarded without treating it
 * as an authenticated sensor observation or altering its signature. */
static void forward_wire(void *context,const uint8_t *frame,size_t length) {
    lane *l=context;
    receiver *s=l->owner;
    if(s->active<0) choose(l);
    if(s->active==(int)l->slot) {
        mr_observe(&l->rates,frame,length,ri_now());
        if(s->config->qgc_enabled) (void)qb_send(&s->bridge,frame,length);
    }
}
static void reset_lane(lane *l) {
    mi_init(&l->decoder,&l->owner->options,forward_event,l);
    qb_parser_init(&l->wire_parser);
    mr_init(&l->rates,l->owner->config->mavlink_rates_enabled && l->serial_baud!=0,l->serial_baud);
    l->tx_len=l->tx_offset=0;
}
static void publish(receiver *s,ri_status_fn status,void *context,const char *state) {
    lane *l=&s->lane[s->active>=0 ? (unsigned)s->active : s->view];
    if(state) (void)snprintf(s->report.status,sizeof(s->report.status),"%s",state);
    (void)snprintf(s->report.source,sizeof(s->report.source),"%s",l->source);
    s->report.serial_baud=l->serial_baud;
    s->report.source_selected=s->active>=0;
    s->report.qgc=s->bridge.report;
    s->report.rates=l->rates.report;
    (void)snprintf(s->report.qgc_status,sizeof(s->report.qgc_status),"%s",
        !s->config->qgc_enabled ? "DISABLED" :
        s->active<0 || !s->lane[s->active].opened || s->lane[s->active].transport.kind!=IT_SERIAL ? "WAIT_SERIAL" :
        s->bridge.report.error[0] || !s->bridge.report.opened ? "ERROR" : "READY");
    mi_get_report(&l->decoder,&s->report.mav);
    if(status) status(context,&s->report);
}
static int open_lane(receiver *s,lane *l,it_kind kind,const char *endpoint,unsigned baud,const tb_config *c,
                     ri_status_fn status,void *context) {
    unsigned timeout=c->io_timeout_ms<RI_POLL_MS ? c->io_timeout_ms : RI_POLL_MS;
    s->view=l->slot;
    if(kind==IT_WEBSOCKET) cfg_safe_url(endpoint,l->source,sizeof(l->source));
    else if(kind==IT_UDP) (void)snprintf(l->source,sizeof(l->source),"udp://%.240s",endpoint);
    else (void)snprintf(l->source,sizeof(l->source),"%.255s",endpoint);
    l->serial_baud=kind==IT_SERIAL ? baud : 0;
    ++s->report.attempts;
    publish(s,status,context,"CONNECTING");
    if(!it_open(&l->transport,kind,endpoint,baud,c->connect_timeout_ms,timeout)) {
        (void)snprintf(s->report.error,sizeof(s->report.error),"%.159s",it_error(&l->transport));
        publish(s,status,context,"CONNECT_FAILED");
        return 0;
    }
    reset_lane(l);
    /* Automatic serial probes must not transmit into an unidentified device,
     * including a preferred port while trying alternate baud rates. */
    l->trusted=kind==IT_WEBSOCKET || (kind==IT_SERIAL && !c->auto_scan && !strcmp(endpoint,c->serial_port));
    l->opened=1;
    l->opened_at=l->last_valid=l->next_heartbeat=ri_now();
    s->report.error[0]=0;
    publish(s,status,context,"PROBING");
    return 1;
}
static const char *selected_status(const mi_report *r,uint64_t now,unsigned timeout_ms) {
    const mi_device *d=mi_selected_device(r);
    if(!d || !d->have_heartbeat) return "WAIT_HEARTBEAT";
    return now>=d->last_heartbeat_ms && now-d->last_heartbeat_ms<timeout_ms ? "LOCKED" : "HEARTBEAT_LOST";
}
static void close_lane(lane *l) {
    if(l->opened && l->owner->active==(int)l->slot && l->transport.kind==IT_SERIAL) {
        if(l->tx_len && l->tx_kind==RI_TX_QGC) ++l->owner->bridge.report.dropped;
        qb_close(&l->owner->bridge);
        l->owner->qgc_retry_at=0;
    }
    if(l->opened) it_close(&l->transport);
    l->opened=0;
    l->tx_len=l->tx_offset=0;
}
static void restart_discovery(receiver *s,uint64_t now) {
    unsigned i;
    s->candidate_count=s->candidate_next=s->baud_pass=0;
    for(i=0;i<RI_LANE_COUNT;++i) {
        close_lane(&s->lane[i]); reset_lane(&s->lane[i]);
        s->lane[i].retry_at=now;
    }
    s->active=-1;
}
static void close_other_lanes(receiver *s,const lane *selected) {
    unsigned i;
    for(i=0;i<RI_LANE_COUNT;++i) if(&s->lane[i]!=selected) close_lane(&s->lane[i]);
}
int ri_run(const tb_config *c,mi_emit_fn emit,ri_status_fn status,ri_stop_fn stop,void *context) {
    receiver *s=&input;
    uint64_t started=ri_now(),next_status=0;
    unsigned i;
    memset(s,0,sizeof(*s));
    s->config=c; qb_init(&s->bridge);
    s->active=-1; s->emit=emit; s->context=context;
    mi_options_default(&s->options);
    s->options.target_system=(uint8_t)c->target_system; s->options.target_component=(uint8_t)c->target_component;
    s->options.origin_auto=(uint8_t)c->origin_auto;
    s->options.origin_lat=c->origin[0]; s->options.origin_lon=c->origin[1]; s->options.origin_alt=c->origin[2];
    s->options.system_time_uncert_us=c->system_time_uncert_us; s->options.accel_bound_mps2=c->accel_bound_mps2;
    for(i=0;i<RI_LANE_COUNT;++i) { s->lane[i].owner=s; s->lane[i].slot=(ri_lane_id)i; reset_lane(&s->lane[i]); }
    publish(s,status,context,"SEARCHING");
    for(;;) {
        uint64_t now=ri_now();
        int worked=0;
        if((stop && stop(context)) || (c->duration_ms && now-started>=c->duration_ms)) break;
        /* UDP remains open while serial/WebSocket candidates are probed. A
         * silent serial device cannot postpone the UDP probe by probe_ms. */
        if(s->active<0 && c->udp_enabled && !s->lane[RI_UDP_LANE].opened && now>=s->lane[RI_UDP_LANE].retry_at) {
            lane *l=&s->lane[RI_UDP_LANE];
            char endpoint[96];
            (void)snprintf(endpoint,sizeof(endpoint),"%s:%u",c->udp_bind,c->udp_port);
            if(!open_lane(s,l,IT_UDP,endpoint,0,c,status,context)) schedule_retry(s,l,c,ri_now());
        }
        if(s->active<0 && !s->lane[RI_ROTATING_LANE].opened && now>=s->lane[RI_ROTATING_LANE].retry_at) {
            lane *l=&s->lane[RI_ROTATING_LANE];
            if(!s->candidate_count) s->candidate_count=candidates(s,c);
            if(s->candidate_count) {
                candidate *current=&s->candidate[s->candidate_next++];
                unsigned baud=current->kind==IT_SERIAL ? serial_baud_probe(c->serial_baud,s->baud_pass) : 0;
                if(!open_lane(s,l,current->kind,current->endpoint,baud,c,status,context)) schedule_retry(s,l,c,ri_now());
            } else {
                schedule_retry(s,l,c,ri_now());
                if(!s->lane[RI_UDP_LANE].opened && !s->report.error[0]) publish(s,status,context,"NO_CHANNEL");
            }
        }
        for(i=0;i<RI_LANE_COUNT;++i) {
            lane *l=&s->lane[i];
            uint64_t before;
            size_t wire_frames=0;
            int n,locked;
            if(!l->opened || (s->active>=0 && s->active!=(int)i)) continue;
            worked=1;
            before=l->decoder.report.frames;
            n=it_read(&l->transport,s->bytes,sizeof(s->bytes));
            now=ri_now();
            if(n>0) {
                s->report.total_bytes+=(unsigned)n;
                mi_feed(&l->decoder,s->bytes,(size_t)n,now);
                if((c->qgc_enabled || c->mavlink_rates_enabled) && l->transport.kind==IT_SERIAL)
                    wire_frames=qb_feed(&l->wire_parser,s->bytes,(size_t)n,forward_wire,l);
                s->report.total_frames+=wire_frames>l->decoder.report.frames-before ?
                    wire_frames : l->decoder.report.frames-before;
            }
            if(l->decoder.report.frames>before || wire_frames) {
                if(s->active<0) choose(l);
                l->last_valid=now;
            }
            locked=s->active==(int)i;
            if(locked) {
                /* The losing source is closed, not decoded or forwarded. */
                close_other_lanes(s,l);
                mi_tick(&l->decoder,now);
                if(l->transport.kind==IT_SERIAL) mr_tick(&l->rates,mi_selected_device(&l->decoder.report),
                    l->decoder.report.reboots,now,c->link_timeout_ms);
                if(l->transport.kind==IT_SERIAL && c->qgc_enabled) {
                    ensure_qgc(s,now);
                    qb_poll(&s->bridge);
                }
            }
            if(!locked && l->slot==RI_UDP_LANE && n>=0 && now-l->opened_at>=c->probe_ms) {
                /* Keep the UDP port bound while releasing a peer that supplied
                 * only noise. Its partial packet must never join the next peer. */
                it_udp_probe_reset(&l->transport);
                reset_lane(l);
                l->opened_at=now;
            }
            if(n<0 || (!locked && l->slot!=RI_UDP_LANE && now-l->opened_at>=c->probe_ms) ||
               (locked && now-l->last_valid>=c->link_timeout_ms)) {
                s->view=i;
                (void)snprintf(s->report.error,sizeof(s->report.error),"%.159s",n<0 ? it_error(&l->transport) :
                               locked ? "MAVLink timeout" : "No valid MAVLink frame on channel");
                if(locked) mi_finish(&l->decoder,now);
                close_lane(l);
                publish(s,status,context,"DISCONNECTED");
                if(locked) {
                    /* Start a new, isolated discovery epoch after link loss. */
                    restart_discovery(s,now);
                } else schedule_retry(s,l,c,now);
                continue;
            }
            /* UDP never announces a GCS, changes streams, or sends commands. */
            /* One frame owns serial output until every byte has been written.
             * Heartbeats, QGC requests and rate commands cannot interleave. */
            if(locked && l->transport.kind==IT_SERIAL && !l->tx_len) {
                l->tx_len=qb_pop(&s->bridge,l->tx,sizeof(l->tx));
                if(l->tx_len) {
                    l->tx_offset=0; l->tx_kind=RI_TX_QGC;
                    mr_external(&l->rates,l->tx,l->tx_len,now);
                }
            }
            if(l->transport.kind!=IT_UDP && (locked || l->trusted) && now>=l->next_heartbeat && !l->tx_len) {
                l->tx_kind=RI_TX_HEARTBEAT;
                l->tx_len=mi_heartbeat(l->tx,sizeof(l->tx),255,191,l->sequence++);
                l->tx_offset=0; l->next_heartbeat=now+RI_HEARTBEAT_MS;
            }
            if(locked && l->transport.kind==IT_SERIAL && !l->tx_len) {
                l->tx_len=mr_next(&l->rates,l->sequence,l->tx,sizeof(l->tx),now);
                if(l->tx_len) { ++l->sequence; l->tx_offset=0; l->tx_kind=RI_TX_RATE; }
            }
            if(l->tx_len) {
                int nout=it_write(&l->transport,l->tx+l->tx_offset,l->tx_len-l->tx_offset);
                if(nout>0) l->tx_offset+=(size_t)nout;
                if(nout<0) {
                    (void)snprintf(s->report.error,sizeof(s->report.error),"%.159s",it_error(&l->transport));
                    if(l->tx_kind==RI_TX_QGC) ++s->bridge.report.errors;
                    if(locked) mi_finish(&l->decoder,now);
                    close_lane(l);
                    publish(s,status,context,"DISCONNECTED");
                    if(locked) restart_discovery(s,now);
                    else schedule_retry(s,l,c,now);
                } else if(l->tx_offset==l->tx_len) {
                    if(l->tx_kind==RI_TX_QGC) qb_delivered(&s->bridge,l->tx_len);
                    if(l->tx_kind==RI_TX_RATE) mr_sent(&l->rates,now);
                    l->tx_len=l->tx_offset=0;
                }
            }
        }
        now=ri_now();
        if(now>=next_status) {
            const char *state;
            if(s->active>=0) state=selected_status(&s->lane[s->active].decoder.report,now,c->link_timeout_ms);
            else state=s->lane[RI_UDP_LANE].opened || s->lane[RI_ROTATING_LANE].opened ? "PROBING" : NULL;
            publish(s,status,context,state);
            next_status=now+RI_STATUS_MS;
        }
        if(!worked) ri_pause(RI_POLL_MS);
    }
    if(s->active>=0) mi_finish(&s->lane[s->active].decoder,ri_now());
    for(i=0;i<RI_LANE_COUNT;++i) close_lane(&s->lane[i]);
    qb_close(&s->bridge);
    publish(s,status,context,"STOPPED");
    return 1;
}
