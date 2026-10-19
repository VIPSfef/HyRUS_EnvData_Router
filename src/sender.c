/* HTTP ACK contract: 204 means every request batch was committed by the server. */
/* Automatic MAVLink receiver or explicit CSV replay -> bounded 1 Hz batches. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "telemetry.h"
#include "time_sync.h"
#include "config.h"
#include "monitor.h"
#include "router_input.h"
#include <signal.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#include <bcrypt.h>
typedef CRITICAL_SECTION mutex_t;
static void lock(mutex_t *m) { EnterCriticalSection(m); }
static void unlock(mutex_t *m) { LeaveCriticalSection(m); }
static uint64_t now_ms(void) { return GetTickCount64(); }
static void pause_ms(unsigned ms) { Sleep(ms); }
#else
#include <pthread.h>
#include <time.h>
#include <curl/curl.h>
#include <sys/random.h>
#include <unistd.h>
typedef pthread_mutex_t mutex_t;
static void lock(mutex_t *m) { (void)pthread_mutex_lock(m); }
static void unlock(mutex_t *m) { (void)pthread_mutex_unlock(m); }
static uint64_t now_ms(void) {
    struct timespec t;
    (void)clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (unsigned long)t.tv_nsec / 1000000;
}
static void pause_ms(unsigned ms) {
    struct timespec t = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000};
    while (nanosleep(&t, &t) && errno == EINTR) {}
}
#endif

#ifndef TB_QUEUE_CAP
#define TB_QUEUE_CAP 8u
#endif
#ifndef TB_SEND_WINDOWS
#define TB_SEND_WINDOWS 4u
#endif
#if TB_QUEUE_CAP < 1 || TB_SEND_WINDOWS < 1 || TB_SEND_WINDOWS > TB_QUEUE_CAP
#error Invalid queue/send window capacities
#endif
#define QUEUE_CAP TB_QUEUE_CAP
#define SEND_WINDOWS TB_SEND_WINDOWS
/* Includes worst-width uint64, subnormal doubles, keys, units and error fields. */
#define BATCH_JSON_CAP (TB_CAPACITY * 512u + TB_MAX_KEYS * 768u + 2048u)
#define WIRE_CAP (SEND_WINDOWS * BATCH_JSON_CAP + 4096u)
typedef struct {
    uint64_t rejected, sequence_gaps, queue_windows, queue_samples, raw_samples;
} losses;
typedef struct {
    char key[TB_KEY_BYTES], unit[TB_UNIT_BYTES];
    uint64_t last_t;
    uint32_t last_seq;
} metric_state;
typedef struct {
    uint64_t seq, generation, captured_ms;
    char session[65], source[256];
    mi_device heartbeat;
    double lat, lon, altitude_msl_m;
    uint64_t navigation_received_ms, navigation_boot_us;
    int have_navigation;
} test_record;
typedef struct {
    mutex_t mutex;
    tb_batch current, queue[QUEUE_CAP];
    weather_batch weather_current, weather_queue[QUEUE_CAP];
    uint8_t queue_weather[QUEUE_CAP];
    uint64_t weather_last_seal_ms;
    unsigned head, count;
    uint64_t last_t, watermark_us, sealed_window;
    metric_state metric[TB_MAX_KEYS];
    unsigned metric_n;
    tc_clock clock;
    int have_last, have_sealed, eof, failed, ready, summary;
    losses loss;
    const char *url, *stream, *session;
    double origin[3];
    tb_config config;
    tm_snapshot diag;
    uint64_t queued_at[QUEUE_CAP];
    const char *config_path;
    int monitor_enabled, live, test_mode;
    test_record test_queue[QUEUE_CAP];
    uint64_t test_next_seq, test_last_heartbeat, test_last_enqueue_ms, test_epoch_start_ms;
    char active_session[65], queue_session[QUEUE_CAP][65];
    uint8_t source_system, source_component, queue_system[QUEUE_CAP], queue_component[QUEUE_CAP];
    uint32_t route_generation, queue_generation[QUEUE_CAP];
    uint64_t run_nonce, next_batch_id, queue_batch_id[QUEUE_CAP], last_seal_ms;
    double queue_origin[QUEUE_CAP][3];
    tc_report queue_clock[QUEUE_CAP];
    int queue_have_clock[QUEUE_CAP], queue_origin_known[QUEUE_CAP];
    int origin_known;
    uint64_t raw_clock_boot,raw_clock_utc,queue_raw_boot[QUEUE_CAP],queue_raw_utc[QUEUE_CAP];
    uint32_t raw_clock_uncert,queue_raw_uncert[QUEUE_CAP];
} app;
static app state; /* Deliberately fixed storage; no large thread-stack objects. */
static char wire[WIRE_CAP];
static char bundle_wire[WIRE_CAP]; /* immutable mixed-type 1 Hz envelope */
static char default_config_path[4096];
static int config_exists(const char *path) {
#ifdef _WIN32
    wchar_t wide[4096]; DWORD attributes;
    if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path,-1,wide,4096)) return 0;
    attributes=GetFileAttributesW(wide);
    return attributes!=INVALID_FILE_ATTRIBUTES && !(attributes&FILE_ATTRIBUTE_DIRECTORY);
#else
    FILE *file=fopen(path,"rb"); if(!file) return 0; fclose(file); return 1;
#endif
}
static const char *default_config(void) {
    size_t len; char *slash;
    if(config_exists("telemetry.ini")) return "telemetry.ini";
#ifdef _WIN32
    wchar_t wide[4096];
    DWORD n=GetModuleFileNameW(NULL,wide,4096);
    if(!n || n>=4096 || !WideCharToMultiByte(CP_UTF8,0,wide,-1,default_config_path,sizeof(default_config_path),NULL,NULL)) return "telemetry.ini";
#else
    ssize_t n=readlink("/proc/self/exe",default_config_path,sizeof(default_config_path)-1);
    if(n<=0 || (size_t)n>=sizeof(default_config_path)-1) return "telemetry.ini";
    default_config_path[n]=0;
#endif
    slash=strrchr(default_config_path,'/');
#ifdef _WIN32
    { char *back=strrchr(default_config_path,'\\'); if(back && (!slash || back>slash)) slash=back; }
#endif
    if(!slash) return "telemetry.ini";
    len=(size_t)(slash-default_config_path)+1;
    if(len+sizeof("telemetry.ini")>sizeof(default_config_path)) return "telemetry.ini";
    memcpy(default_config_path+len,"telemetry.ini",sizeof("telemetry.ini"));
    return default_config_path;
}

#ifdef _WIN32
static volatile LONG stop_requested;
static BOOL WINAPI input_control(DWORD event) {
    if(event==CTRL_C_EVENT || event==CTRL_BREAK_EVENT || event==CTRL_CLOSE_EVENT || event==CTRL_SHUTDOWN_EVENT) {
        (void)InterlockedExchange(&stop_requested,1); return TRUE;
    }
    return FALSE;
}
#else
static volatile sig_atomic_t stop_requested;
static void input_control(int event) { (void)event; stop_requested=1; }
#endif
static int input_stop(void *context) {
    app *a=context; int stop;
    lock(&a->mutex); stop=a->failed; unlock(&a->mutex);
#ifdef _WIN32
    return InterlockedCompareExchange(&stop_requested,0,0)!=0 || stop;
#else
    return stop_requested || stop;
#endif
}
static void input_signals(void) {
#ifdef _WIN32
    (void)SetConsoleCtrlHandler(input_control,TRUE);
#else
    struct sigaction action; memset(&action,0,sizeof(action));
    action.sa_handler=input_control; (void)sigemptyset(&action.sa_mask);
    (void)sigaction(SIGINT,&action,NULL); (void)sigaction(SIGTERM,&action,NULL);
#endif
}
static int run_nonce(uint64_t *value) {
#ifdef _WIN32
    return BCryptGenRandom(NULL,(PUCHAR)value,sizeof(*value),BCRYPT_USE_SYSTEM_PREFERRED_RNG)>=0;
#else
    return getrandom(value,sizeof(*value),0)==(ssize_t)sizeof(*value);
#endif
}


static int separator(char **p, char c) {
    while (isspace((unsigned char)**p)) ++*p;
    if (**p != c) return 0;
    if (c) ++*p;
    return 1;
}
static int integer(char **p, uint64_t *v, char sep) {
    char *end;
    while (isspace((unsigned char)**p)) ++*p;
    if (!isdigit((unsigned char)**p)) return 0;
    errno = 0;
    *v = strtoull(*p, &end, 10);
    if (errno || end == *p) return 0;
    *p = end;
    return separator(p, sep);
}
static int real_number(char **p, double *v, char sep) {
    char *end;
    errno = 0;
    *v = strtod(*p, &end);
    if (end == *p || errno == ERANGE) return 0;
    *p = end;
    return separator(p, sep);
}
static int name_field(char **p, char *out, size_t cap) {
    size_t n = 0;
    while (**p && **p != ',' && !isspace((unsigned char)**p)) {
        if (n + 1 >= cap) return 0;
        out[n++] = *(*p)++;
    }
    out[n] = 0;
    return n && separator(p, ',');
}
static int parse(char *p, tb_sample *s) {
    uint64_t u;
    double d;
    unsigned i;
    memset(s, 0, sizeof(*s));
    if (!integer(&p, &s->t_us, ',')) return 0;
    if (!integer(&p, &u, ',') || u > UINT32_MAX) return 0;
    s->seq = (uint32_t)u;
    if (!integer(&p, &u, ',') || u > UINT32_MAX) return 0;
    s->quality = (uint32_t)u;
    if (!name_field(&p, s->key, sizeof(s->key)) ||
        !name_field(&p, s->unit, sizeof(s->unit)) ||
        !real_number(&p, &s->value, ',')) return 0;
    for (i = 0; i < 3; ++i) if (!real_number(&p, &s->p[i], ',')) return 0;
    for (i = 0; i < 3; ++i) {
        if (!real_number(&p, &d, ',') || (isfinite(d) && fabs(d) > 1e9)) return 0;
        s->v[i] = (float)d;
    }
    if (!integer(&p, &u, ',') || u > UINT32_MAX) return 0;
    s->time_uncert_us = (uint32_t)u;
    if (!integer(&p, &u, ',') || u > UINT32_MAX) return 0;
    s->nav_age_us = (uint32_t)u;
    if (!real_number(&p, &d, '\0') || !isfinite(d) || d < 0 || d > 1e9) return 0;
    s->pos_error_m = (float)d;
    return tb_sample_valid(s);
}
/* Caller holds mutex. Full queue drops NEW window, never overwrites in-flight data. */
static void seal(app *a) {
    if (!a->current.n) return;
    if (a->count == QUEUE_CAP) {
        ++a->loss.queue_windows;
        a->loss.queue_samples += a->current.n;
    } else {
        unsigned slot = (a->head + a->count) % QUEUE_CAP;
        a->queue[slot] = a->current;
        a->queue_weather[slot]=0;
        a->queued_at[slot] = now_ms();
        (void)snprintf(a->queue_session[slot],sizeof(a->queue_session[slot]),"%s",a->live ? a->active_session : a->session);
        memcpy(a->queue_origin[slot],a->origin,sizeof(a->origin));
        a->queue_origin_known[slot]=a->origin_known;
        a->queue_system[slot]=a->source_system; a->queue_component[slot]=a->source_component;
        a->queue_generation[slot]=a->route_generation;
        a->queue_batch_id[slot]=++a->next_batch_id;
        a->queue_have_clock[slot]=tc_get_report(&a->clock,&a->queue_clock[slot]);
        a->queue_raw_boot[slot]=a->raw_clock_boot; a->queue_raw_utc[slot]=a->raw_clock_utc;
        a->queue_raw_uncert[slot]=a->raw_clock_uncert;
        ++a->count;
    }
    a->sealed_window = a->current.window_us;
    a->have_sealed = 1;
    a->current.n = 0;
    a->last_seal_ms=now_ms();
}
/* Same immutable ring and HTTP204 release contract for grouped SI weather.
 * A sender interval seals a host collection window; it is NOT sensor time. */
static void seal_weather(app *a) {
    unsigned slot;
    if(!a->weather_current.n) return;
    if(a->count==QUEUE_CAP) {
        ++a->loss.queue_windows; a->loss.queue_samples+=a->weather_current.n;
    } else {
        slot=(a->head+a->count)%QUEUE_CAP;
        a->weather_queue[slot]=a->weather_current; a->queue_weather[slot]=1;
        a->queued_at[slot]=now_ms();
        (void)snprintf(a->queue_session[slot],sizeof(a->queue_session[slot]),"%s",a->active_session);
        memcpy(a->queue_origin[slot],a->origin,sizeof(a->origin));
        a->queue_origin_known[slot]=a->origin_known;
        a->queue_system[slot]=a->source_system; a->queue_component[slot]=a->source_component;
        a->queue_generation[slot]=a->route_generation;
        a->queue_batch_id[slot]=++a->next_batch_id;
        a->queue_have_clock[slot]=tc_get_report(&a->clock,&a->queue_clock[slot]);
        ++a->count;
    }
    weather_batch_init(&a->weather_current,now_ms());
    a->weather_last_seal_ms=now_ms();
}
static void ingest_weather(app *a,const mi_event *e) {
    const weather_sample *s=&e->weather; unsigned k;
    lock(&a->mutex);
    a->source_system=e->sysid; a->source_component=e->compid;
    if(!a->weather_current.n) weather_batch_init(&a->weather_current,now_ms());
    if(weather_batch_add(&a->weather_current,s)) {
        ++a->diag.accepted; a->diag.last_receipt_ms=now_ms();
        if(a->weather_current.n>a->weather_current.raw_n) ++a->loss.raw_samples;
        /* TUI overview aggregates metric counts only. Wire groups retain each
         * sensor's identity and independent statistics. */
        for(k=0;k<a->diag.key_n && strcmp(a->diag.keys[k].key,s->metric==WEATHER_TEMPERATURE ? "hyg_temperature" : "hyg_humidity");++k) {}
        if(k<TB_MAX_KEYS) {
            tm_key *key=&a->diag.keys[k];
            if(k==a->diag.key_n) { memset(key,0,sizeof(*key)); ++a->diag.key_n; }
            (void)strcpy(key->key,s->metric==WEATHER_TEMPERATURE ? "hyg_temperature" : "hyg_humidity");
            (void)strcpy(key->unit,s->metric==WEATHER_TEMPERATURE ? "K" : "1");
            ++key->observations; key->t_us=s->has_time ? s->t_us : 0;
            key->seq=s->seq; key->value=s->value; key->have_value=1;
        }
        if(s->has_position) {
            a->diag.have_position=1; memcpy(a->diag.p,s->p,sizeof(s->p)); memcpy(a->diag.v,s->v,sizeof(s->v));
            a->diag.time_uncert_us=s->time_uncert_us; a->diag.pos_error_m=s->pos_error_m;
        }
    } else ++a->loss.rejected;
    unlock(&a->mutex);
}
static void ingest(app *a, const tb_sample *input) {
    tb_sample sample = *input;
    tb_sample *s = &sample;
    uint64_t w = s->t_us / 1000000 * 1000000;
    uint64_t utc;
    uint32_t uncertainty;
    unsigned k;
    lock(&a->mutex);
    for (k = 0; k < a->metric_n && strcmp(a->metric[k].key, s->key); ++k) {}
    if (tc_estimate(&a->clock, s->t_us, &utc, &uncertainty)) {
        s->utc_us = utc;
        s->utc_uncert_us = uncertainty;
    } else {
        s->utc_us = 0;
        s->utc_uncert_us = UINT32_MAX;
    }
    if (!tb_sample_valid(s) || s->t_us < a->watermark_us ||
        (a->have_last && s->t_us < a->last_t) ||
        (k == a->metric_n && k == TB_MAX_KEYS) ||
        (k < a->metric_n && (strcmp(a->metric[k].unit, s->unit) ||
         s->seq <= a->metric[k].last_seq || s->t_us <= a->metric[k].last_t)) ||
        (!a->live && a->have_sealed && w <= a->sealed_window)) {
        ++a->loss.rejected;
    } else {
        if (a->current.n && a->current.window_us != w) seal(a);
        if (!a->current.n) tb_batch_init(&a->current, w);
        if (tb_batch_add(&a->current, s)) {
            if (k < a->metric_n) a->loss.sequence_gaps += (uint64_t)s->seq - a->metric[k].last_seq - 1;
            else {
                (void)strcpy(a->metric[k].key, s->key);
                (void)strcpy(a->metric[k].unit, s->unit);
                ++a->metric_n;
            }
            a->metric[k].last_t = s->t_us;
            a->metric[k].last_seq = s->seq;
            ++a->diag.accepted;
            {
                unsigned row; tm_key *key;
                for(row=0;row<a->diag.key_n && strcmp(a->diag.keys[row].key,s->key);++row) {}
                if(row<TB_MAX_KEYS) {
                if(row==a->diag.key_n) { memset(&a->diag.keys[row],0,sizeof(a->diag.keys[row])); ++a->diag.key_n; }
                key = &a->diag.keys[row];
                (void)strcpy(key->key, s->key);
                (void)strcpy(key->unit, s->unit);
                ++key->observations;
                key->t_us = s->t_us; key->seq = s->seq;
                key->value = s->value; key->have_value = isfinite(s->value);
                }
            }
            a->diag.have_position = 1;
            memcpy(a->diag.p, s->p, sizeof(s->p));
            memcpy(a->diag.v, s->v, sizeof(s->v));
            a->diag.time_uncert_us = s->time_uncert_us;
            a->diag.nav_age_us = s->nav_age_us;
            a->diag.pos_error_m = s->pos_error_m;
            a->diag.utc_us = s->utc_us;
            a->diag.utc_uncert_us = s->utc_uncert_us;
            a->last_t = s->t_us;
            a->have_last = 1;
            if (a->current.n > a->current.raw_n) ++a->loss.raw_samples;
        } else ++a->loss.rejected;
    }
    unlock(&a->mutex);
}
static void mavlink_event(void *context,const mi_event *event) {
    app *a=context;
    if(a->test_mode==1) {
        if(event->kind==MI_RESET) {
            lock(&a->mutex);
            ++a->route_generation;
            a->test_epoch_start_ms=now_ms();
            (void)snprintf(a->active_session,sizeof(a->active_session),"%.28s.%016" PRIx64 ".%u",
                           a->session,a->run_nonce,a->route_generation);
            unlock(&a->mutex);
        }
        return; /* Test mode never uploads weather observations or statistics. */
    }
    if(event->kind==MI_WEATHER_SAMPLE) { ingest_weather(a,event); return; }
    if(event->kind==MI_SAMPLE && a->test_mode!=2) {
        tb_sample sample=event->sample; unsigned k;
        (void)cfg_unit_for_key(&a->config,sample.key,sample.unit,sizeof(sample.unit));
        lock(&a->mutex);
        a->source_system=event->sysid; a->source_component=event->compid;
        for(k=0;k<a->metric_n && strcmp(a->metric[k].key,sample.key);++k) {}
        sample.seq=k<a->metric_n ? a->metric[k].last_seq+1 : 1;
        unlock(&a->mutex);
        ingest(a,&sample);
        return;
    }
    lock(&a->mutex);
    if(event->kind==MI_RESET) {
        seal(a); seal_weather(a);
        a->origin_known=!a->config.origin_auto;
        a->metric_n=0; a->have_last=a->have_sealed=0; a->watermark_us=0;
        a->source_system=event->sysid; a->source_component=event->compid;
        a->diag.key_n=0; memset(a->diag.keys,0,sizeof(a->diag.keys));
        a->diag.have_position=0; tc_init(&a->clock);
        a->raw_clock_boot=a->raw_clock_utc=0; a->raw_clock_uncert=UINT32_MAX;
        ++a->route_generation;
        (void)snprintf(a->active_session,sizeof(a->active_session),"%.28s.%016" PRIx64 ".%u",
                       a->session,a->run_nonce,a->route_generation);
    } else if(event->kind==MI_ORIGIN) {
        memcpy(a->origin,event->origin,sizeof(a->origin)); a->origin_known=1;
    } else if(event->kind==MI_CLOCK) {
        a->raw_clock_boot=event->boot_us; a->raw_clock_utc=event->utc_us; a->raw_clock_uncert=event->uncert_us;
        if(event->uncert_us!=UINT32_MAX && tc_anchor(&a->clock,event->boot_us,event->utc_us,event->uncert_us,0))
            a->diag.last_anchor_receipt_ms=now_ms();
    } else if(event->kind==MI_WATERMARK && event->boot_us>=a->watermark_us) {
        a->watermark_us=event->boot_us;
        if(a->current.n && event->boot_us>=a->current.window_us &&
           event->boot_us-a->current.window_us>=TB_WINDOW_US) seal(a);
    }
    unlock(&a->mutex);
}
/* Called under app mutex. Sample only a new, identified heartbeat, at most
 * once per configured send interval. A coordinate is the last real navigation
 * sample with its own source boot time and age, not a reconstructed HB fix. */
static void capture_test_record(app *a,const ri_report *r) {
    unsigned i,slot;
    uint64_t now=now_ms();
    const mi_device *device=NULL;
    test_record *record;
    if(strcmp(r->status,"LOCKED")) return;
    for(i=0;i<r->mav.devices;++i) {
        const mi_device *candidate=&r->mav.device[i];
        if(candidate->have_heartbeat && candidate->sysid==r->mav.selected_system &&
           candidate->compid==r->mav.selected_component) { device=candidate; break; }
    }
    if(!device || device->last_heartbeat_ms<=a->test_epoch_start_ms ||
       device->last_heartbeat_ms==a->test_last_heartbeat ||
       now-device->last_heartbeat_ms>=a->config.link_timeout_ms ||
       (a->test_last_enqueue_ms && now-a->test_last_enqueue_ms<a->config.send_interval_ms)) return;
    a->test_last_heartbeat=device->last_heartbeat_ms;
    a->test_last_enqueue_ms=now;
    if(a->count==QUEUE_CAP) { ++a->loss.queue_windows; ++a->loss.queue_samples; return; }
    slot=(a->head+a->count)%QUEUE_CAP;
    record=&a->test_queue[slot]; memset(record,0,sizeof(*record));
    record->seq=++a->test_next_seq; record->generation=a->route_generation;
    record->captured_ms=now; record->heartbeat=*device;
    (void)snprintf(record->session,sizeof(record->session),"%s",a->active_session);
    (void)snprintf(record->source,sizeof(record->source),"%s",r->source);
    record->have_navigation=r->mav.have_navigation && now>=r->mav.navigation_received_ms &&
        now-r->mav.navigation_received_ms<a->config.link_timeout_ms;
    record->lat=r->mav.lat; record->lon=r->mav.lon; record->altitude_msl_m=r->mav.altitude_msl_m;
    record->navigation_received_ms=r->mav.navigation_received_ms;
    record->navigation_boot_us=r->mav.navigation_boot_us;
    a->queued_at[slot]=now; ++a->count; ++a->diag.accepted;
    a->diag.last_receipt_ms=now;
}
static void input_status(void *context,const ri_report *report) {
    app *a=context; unsigned i;
    lock(&a->mutex);
    (void)snprintf(a->diag.active_source,sizeof(a->diag.active_source),"%s",report->source);
    (void)snprintf(a->diag.source_status,sizeof(a->diag.source_status),"%s",report->status);
    (void)snprintf(a->diag.source_error,sizeof(a->diag.source_error),"%s",report->error);
    /* Discovery can display either lane. Monotonic receiver totals count each
     * read/frame once, without treating a change of the displayed lane as data. */
    a->diag.input_bytes=report->total_bytes;
    a->diag.input_lines=report->total_frames;
    if(a->test_mode==1) capture_test_record(a,report);
    a->diag.parsed_frames=report->mav.frames; a->diag.bad_crc=report->mav.bad_crc;
    a->diag.unknown_message=report->mav.unknown_message;
    a->diag.no_navigation=report->mav.no_navigation;
    a->diag.pending_overflow=report->mav.pending_overflow;
    a->diag.weather_reboot_dropped=report->mav.weather_reboot_dropped;
    a->diag.signed_rejected=report->mav.signed_rejected;
    a->diag.no_clock=report->mav.no_clock;
    a->diag.wrong_clock_component=report->mav.wrong_clock_component;
    a->diag.invalid_value=report->mav.invalid_value;
    a->diag.late_observation=report->mav.late_observation;
    a->diag.pending_observations=report->mav.pending;
    a->diag.selected_system=report->mav.selected_system;
    a->diag.selected_component=report->mav.selected_component;
    a->diag.gps_fix=report->mav.gps_fix;
    a->diag.device_n=report->mav.devices<TM_MAX_DEVICES ? report->mav.devices : TM_MAX_DEVICES;
    for(i=0;i<a->diag.device_n;++i) {
        const mi_device *d=&report->mav.device[i]; tm_device *out=&a->diag.devices[i];
        out->sysid=d->sysid; out->compid=d->compid; out->type=d->type; out->autopilot=d->autopilot;
        out->base_mode=d->base_mode; out->system_status=d->system_status;
        out->have_heartbeat=d->have_heartbeat;
        out->custom_mode=d->custom_mode; out->last_seen_ms=d->last_seen_ms;
        if(d->last_seen_ms>a->diag.last_receipt_ms) a->diag.last_receipt_ms=d->last_seen_ms;
    }
    unlock(&a->mutex);
}
/* Explicit source watermark: future measurements must have t >= watermark. */
static int control(app *a, char *p) {
    uint64_t t, utc, u, rtt;
    int ok = 0;
    if (!strncmp(p, "@watermark,", 11)) {
        p += 11;
        if (!integer(&p, &t, '\0')) return 0;
        lock(&a->mutex);
        if (t >= a->watermark_us && (!a->have_last || t >= a->last_t)) {
            a->watermark_us = t;
            if (a->current.n && t >= a->current.window_us &&
                t - a->current.window_us >= TB_WINDOW_US) seal(a);
            ok = 1;
        }
        unlock(&a->mutex);
    } else if (!strncmp(p, "@clock,", 7)) {
        p += 7;
        if (!integer(&p, &t, ',') || !integer(&p, &utc, ',') ||
            !integer(&p, &u, ',') || u > UINT32_MAX ||
            !integer(&p, &rtt, '\0') || rtt > UINT32_MAX) return 0;
        lock(&a->mutex);
        ok = tc_anchor(&a->clock, t, utc, (uint32_t)u, (uint32_t)rtt);
        if (ok) a->diag.last_anchor_receipt_ms = now_ms();
        unlock(&a->mutex);
    }
    return ok;
}

#ifdef _WIN32
typedef struct { HINTERNET session, connection; wchar_t route[2048]; DWORD flags; } http;
static int http_open(http *h, const tb_config *config) {
    const char *url = config->url;
    wchar_t wurl[2048], host[256];
    URL_COMPONENTS c;
    memset(h, 0, sizeof(*h));
    memset(&c, 0, sizeof(c));
    c.dwStructSize = sizeof(c);
    c.dwHostNameLength = c.dwUrlPathLength = c.dwExtraInfoLength = (DWORD)-1;
    c.dwUserNameLength = c.dwPasswordLength = (DWORD)-1;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, url, -1, wurl, 2048) ||
        !WinHttpCrackUrl(wurl, 0, 0, &c) || c.dwHostNameLength >= 256 ||
        c.dwUrlPathLength + c.dwExtraInfoLength >= 2047 ||
        c.dwUserNameLength || c.dwPasswordLength ||
        (c.nScheme != INTERNET_SCHEME_HTTP && c.nScheme != INTERNET_SCHEME_HTTPS)) return 0;
    wmemcpy(host, c.lpszHostName, c.dwHostNameLength);
    host[c.dwHostNameLength] = 0;
    if (c.dwUrlPathLength) wmemcpy(h->route, c.lpszUrlPath, c.dwUrlPathLength);
    else h->route[0] = L'/';
    {
        DWORD n = c.dwUrlPathLength ? c.dwUrlPathLength : 1;
        if (c.dwExtraInfoLength) wmemcpy(h->route + n, c.lpszExtraInfo, c.dwExtraInfoLength);
        h->route[n + c.dwExtraInfoLength] = 0;
    }
    h->flags = c.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    h->session = WinHttpOpen(L"mavlink_telemetry_data_router/0.7.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h->session) return 0;
    if (!WinHttpSetTimeouts(h->session, (int)config->connect_timeout_ms,
        (int)config->connect_timeout_ms, (int)config->io_timeout_ms, (int)config->io_timeout_ms)) return 0;
    h->connection = WinHttpConnect(h->session, host, c.nPort, 0);
    return h->connection != NULL;
}
static int http_post(http *h, const tb_config *config, const char *body, size_t n,
                     long *status_out, unsigned *error_out) {
    DWORD status = 0, len = sizeof(status), redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    const char *token = getenv(config->token_env);
    HINTERNET r = WinHttpOpenRequest(h->connection, L"POST", h->route, NULL,
                          WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, h->flags);
    int ok = 0;
    *status_out = 0; *error_out = 0;
    if (!r) { *error_out = (unsigned)GetLastError(); return 0; }
    if (!WinHttpSetOption(r, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects)) ||
        !WinHttpAddRequestHeaders(r, L"Content-Type: application/json\r\n", (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD)) goto done;
    if (token && *token) {
        char header[1152];
        wchar_t wheader[1152];
        if (strlen(token) > 1024 || strpbrk(token, "\r\n")) {
            SetLastError(ERROR_INVALID_PARAMETER); goto done;
        }
        (void)snprintf(header, sizeof(header), "Authorization: Bearer %s\r\n", token);
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, header, -1, wheader, 1152) ||
            !WinHttpAddRequestHeaders(r, wheader, (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD)) goto done;
    }
    ok = WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, (void *)body,
                           (DWORD)n, (DWORD)n, 0) &&
         WinHttpReceiveResponse(r, NULL) &&
         WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX) &&
         status == 204;
done:
    *status_out = (long)status;
    if (!ok && !status) *error_out = (unsigned)GetLastError();
    WinHttpCloseHandle(r);
    return ok;
}
static void http_close(http *h) {
    if (h->connection) WinHttpCloseHandle(h->connection);
    if (h->session) WinHttpCloseHandle(h->session);
}
#else
typedef struct { CURL *easy; struct curl_slist *headers; int global_ready; } http;
static size_t discard(char *p, size_t size, size_t n, void *u) {
    (void)p; (void)u; return size * n;
}
static int http_open(http *h, const tb_config *config) {
    const char *url = config->url;
    const char *token = getenv(config->token_env);
    memset(h, 0, sizeof(*h));
    if (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) return 0;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return 0;
    h->global_ready = 1;
    h->easy = curl_easy_init();
    if (!h->easy) return 0;
    h->headers = curl_slist_append(NULL, "Content-Type: application/json");
    if (!h->headers) return 0;
    if (token && *token) {
        char header[1152];
        struct curl_slist *list;
        if (strlen(token) > 1024 || strpbrk(token, "\r\n")) return 0;
        (void)snprintf(header, sizeof(header), "Authorization: Bearer %s", token);
        list = curl_slist_append(h->headers, header);
        if (!list) return 0;
        h->headers = list;
    }
    curl_easy_setopt(h->easy, CURLOPT_URL, url);
    curl_easy_setopt(h->easy, CURLOPT_HTTPHEADER, h->headers);
    curl_easy_setopt(h->easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h->easy, CURLOPT_CONNECTTIMEOUT_MS, (long)config->connect_timeout_ms);
    curl_easy_setopt(h->easy, CURLOPT_TIMEOUT_MS, (long)config->connect_timeout_ms + 2L * config->io_timeout_ms);
    curl_easy_setopt(h->easy, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(h->easy, CURLOPT_WRITEFUNCTION, discard);
    return 1;
}
static int http_post(http *h, const tb_config *config, const char *body, size_t n,
                     long *status_out, unsigned *error_out) {
    long status = 0;
    CURLcode result;
    (void)config;
    curl_easy_setopt(h->easy, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(h->easy, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)n);
    result = curl_easy_perform(h->easy);
    (void)curl_easy_getinfo(h->easy, CURLINFO_RESPONSE_CODE, &status);
    *status_out = status; *error_out = (unsigned)result;
    return result == CURLE_OK && status == 204;
}
static void http_close(http *h) {
    if (h->easy) curl_easy_cleanup(h->easy);
    curl_slist_free_all(h->headers);
    if (h->global_ready) curl_global_cleanup();
}
#endif

static int clock_json(const tc_report *report,char *out,size_t cap) {
    if(!report) return snprintf(out,cap,"null");
    return snprintf(out,cap,
        "{\"anchors\":%u,\"oldest_boot_us\":\"%" PRIu64 "\",\"last_boot_us\":\"%" PRIu64 "\","
        "\"last_utc_us\":\"%" PRIu64 "\",\"offset_us\":\"%" PRId64 "\","
        "\"drift_ppm\":%.17g,\"residual_std_us\":%.17g,\"max_residual_us\":%.17g,"
        "\"anchor_uncert_max_us\":%u,\"rtt_mean_us\":%.17g,\"rtt_max_us\":%u}",
        (unsigned)report->count,report->oldest_boot_us,report->last_boot_us,
        report->last_utc_us,report->offset_us,report->drift_ppm,
        report->residual_std_us,report->max_abs_residual_us,
        (unsigned)report->anchor_uncert_max_us,report->roundtrip_mean_us,
        (unsigned)report->roundtrip_max_us);
}

static int test_append(size_t *used,const char *format,...) {
    va_list args; int n;
    va_start(args,format);
    n=vsnprintf(wire+*used,sizeof(wire)-*used,format,args);
    va_end(args);
    if(n<0 || (size_t)n>=sizeof(wire)-*used) return 0;
    *used+=(size_t)n; return 1;
}
static int test_quote(size_t *used,const char *value) {
    const unsigned char *p=(const unsigned char *)value;
    if(!test_append(used,"\"")) return 0;
    for(;*p;++p) {
        if(*p<32 || *p=='\"' || *p=='\\') {
            if(!test_append(used,"\\u%04x",(unsigned)*p)) return 0;
        } else if(!test_append(used,"%c",*p)) return 0;
    }
    return test_append(used,"\"");
}
static int encode_test(app *a,unsigned head,unsigned n) {
    size_t used=0; unsigned i;
    if(!test_append(&used,"{\"schema\":\"mavlink_router_test_v1\",\"stream\":") ||
       !test_quote(&used,a->stream) || !test_append(&used,",\"records\":[")) return 0;
    for(i=0;i<n;++i) {
        const test_record *r=&a->test_queue[(head+i)%QUEUE_CAP];
        const mi_device *d=&r->heartbeat;
        if(!test_append(&used,"%s{\"seq\":%" PRIu64 ",\"source_generation\":%" PRIu64 ",\"session\":",i ? "," : "",r->seq,r->generation) ||
           !test_quote(&used,r->session) || !test_append(&used,",\"source\":") || !test_quote(&used,r->source) ||
           !test_append(&used,",\"heartbeat\":{\"sysid\":%u,\"compid\":%u,\"type\":%u,\"autopilot\":%u,"
             "\"base_mode\":%u,\"custom_mode\":%u,\"system_status\":%u,\"received_monotonic_ms\":%" PRIu64 "},\"position\":",
             (unsigned)d->sysid,(unsigned)d->compid,(unsigned)d->type,(unsigned)d->autopilot,
             (unsigned)d->base_mode,(unsigned)d->custom_mode,(unsigned)d->system_status,d->last_heartbeat_ms)) return 0;
        if(r->have_navigation) {
            if(!test_append(&used,"{\"lat_deg\":%.17g,\"lon_deg\":%.17g,\"alt_msl_m\":%.17g,"
                "\"boot_ms\":%" PRIu64 ",\"received_monotonic_ms\":%" PRIu64 "},\"coordinate_age_ms\":%" PRIu64 "}",
                r->lat,r->lon,r->altitude_msl_m,r->navigation_boot_us/1000,r->navigation_received_ms,
                r->captured_ms>=r->navigation_received_ms ? r->captured_ms-r->navigation_received_ms : 0)) return 0;
        } else if(!test_append(&used,"null,\"coordinate_age_ms\":null}")) return 0;
    }
    return test_append(&used,"]}\n") ? (int)used : 0;
}
static int encode_weather(app *a,unsigned head) {
    size_t used=0; int n; const double *origin=a->queue_origin[head];
    if(!test_append(&used,"{\"schema\":\"mavlink_weather_v1\",\"stream\":") || !test_quote(&used,a->stream) ||
       !test_append(&used,",\"records\":[{\"seq\":%" PRIu64 ",\"generation\":%u,\"session\":",a->queue_batch_id[head],a->queue_generation[head]) ||
       !test_quote(&used,a->queue_session[head]) ||
       !test_append(&used,",\"source\":{\"sysid\":%u,\"compid\":%u},\"frame\":\"ENU\",\"origin_wgs84\":[%.17g,%.17g,%.17g],"
         "\"time_basis\":\"pixhawk_boot_us\",\"host_time_basis\":\"collector_monotonic_ms\",\"clock_model\":",
         (unsigned)a->queue_system[head],(unsigned)a->queue_component[head],origin[0],origin[1],origin[2])) return 0;
    if(!a->queue_origin_known[head]) {
        char *start=strstr(wire,"\"origin_wgs84\":["), *end;
        if(start && (end=strchr(start,']'))!=NULL) {
            start+=15; /* retain the colon; replace the numeric array */
            memmove(start+4,end+1,used-(size_t)(end+1-wire)+1);
            memcpy(start,"null",4); used=strlen(wire);
        }
    }
    n=clock_json(a->queue_have_clock[head] ? &a->queue_clock[head] : NULL,wire+used,sizeof(wire)-used);
    if(n<0 || (size_t)n>=sizeof(wire)-used) return 0;
    used+=(size_t)n;
    if(!test_append(&used,",\"weather\":")) return 0;
    weather_batch_prepare(&a->weather_queue[head]);
    n=weather_batch_json(&a->weather_queue[head],wire+used,sizeof(wire)-used);
    if(n<0) return 0;
    used+=(size_t)n;
    return test_append(&used,"}]}\n") ? (int)used : 0;
}
static int encode(app *a, unsigned head, unsigned n, losses l, const tc_report *report) {
    unsigned i;
    int k;
    size_t used;
    const double *origin=a->live ? a->queue_origin[head] : a->origin;
    k = snprintf(wire, sizeof(wire),
        "{\"schema\":%u,\"stream\":\"%s\",\"session\":\"%s\",\"frame\":\"ENU\","
        "\"origin_wgs84\":[%.17g,%.17g,%.17g],\"time_basis\":\"pixhawk_boot_us\","
        "\"time_epoch\":\"session_boot\",\"utc_basis\":\"unix_us_estimate\","
        "\"field_order\":[\"t_us\",\"seq\",\"quality\",\"key\",\"unit\",\"east_m\",\"north_m\",\"up_m\","
        "\"ve_mps\",\"vn_mps\",\"vu_mps\",\"value\",\"time_uncert_us\",\"nav_age_us\",\"pos_error_m\",\"utc_us\",\"utc_uncert_us\"],"
        "\"loss\":{\"rejected\":\"%" PRIu64 "\",\"sequence_gaps\":\"%" PRIu64 "\","
        "\"queue_windows\":\"%" PRIu64 "\",\"queue_samples\":\"%" PRIu64 "\",\"raw_samples\":\"%" PRIu64 "\"},\"clock_model\":",
        a->live ? 3u : 2u, a->stream, a->live ? a->queue_session[head] : a->session, origin[0], origin[1], origin[2],
        l.rejected, l.sequence_gaps, l.queue_windows, l.queue_samples, l.raw_samples);
    if (k < 0 || (size_t)k >= sizeof(wire)) return 0;
    used = (size_t)k;
    k=clock_json(report,wire+used,sizeof(wire)-used);
    if (k < 0 || (size_t)k >= sizeof(wire) - used) return 0;
    used += (size_t)k;
    k=snprintf(wire+used,sizeof(wire)-used,",\"batches\":[");
    if(k<0 || (size_t)k>=sizeof(wire)-used) return 0;
    used+=(size_t)k;
    if(a->live) {
        /* Insert source identity before the batches member without mutable state. */
        const char *suffix="\"batches\":[";
        size_t tail=strlen(suffix);
        if(used<tail) return 0;
        used-=tail;
        k=snprintf(wire+used,sizeof(wire)-used,
          "\"source\":{\"sysid\":%u,\"compid\":%u,\"epoch\":%u,\"altitude_datum\":\"MSL\"},\"batches\":[",
          (unsigned)a->queue_system[head],(unsigned)a->queue_component[head],a->queue_generation[head]);
        if(k<0 || (size_t)k>=sizeof(wire)-used) return 0;
        used+=(size_t)k;
        if(a->queue_raw_utc[head]) {
            char uncertainty[32];
            if(a->queue_raw_uncert[head]==UINT32_MAX) (void)strcpy(uncertainty,"null");
            else (void)snprintf(uncertainty,sizeof(uncertainty),"%u",a->queue_raw_uncert[head]);
            used-=strlen("\"batches\":[");
            k=snprintf(wire+used,sizeof(wire)-used,"\"raw_clock_anchor\":{\"boot_us\":\"%" PRIu64 "\",\"utc_us\":\"%" PRIu64 "\",\"uncert_us\":%s},\"batches\":[",
                a->queue_raw_boot[head],a->queue_raw_utc[head],uncertainty);
            if(k<0 || (size_t)k>=sizeof(wire)-used) return 0;
            used+=(size_t)k;
        }
    }
    for (i = 0; i < n; ++i) {
        if (i) wire[used++] = ',';
        tb_batch_prepare(&a->queue[(head + i) % QUEUE_CAP]);
        k = tb_batch_json_mode(&a->queue[(head + i) % QUEUE_CAP], wire + used,
                               sizeof(wire) - used, !a->summary);
        if (k < 0) return 0;
        if(a->live) {
            char prefix[1536];
            unsigned slot=(head+i)%QUEUE_CAP;
            int part,prefix_n=snprintf(prefix,sizeof(prefix),"{\"batch_id\":\"%" PRIu64 "\",\"clock_model\":",a->queue_batch_id[slot]);
            if(prefix_n<1 || (size_t)prefix_n>=sizeof(prefix)) return 0;
            part=clock_json(a->queue_have_clock[slot] ? &a->queue_clock[slot] : NULL,
                            prefix+prefix_n,sizeof(prefix)-(size_t)prefix_n);
            if(part<0 || (size_t)part>=sizeof(prefix)-(size_t)prefix_n) return 0;
            prefix_n+=part;
            if(a->queue_raw_utc[slot]) {
                char uncertainty[32];
                if(a->queue_raw_uncert[slot]==UINT32_MAX) (void)strcpy(uncertainty,"null");
                else (void)snprintf(uncertainty,sizeof(uncertainty),"%u",a->queue_raw_uncert[slot]);
                part=snprintf(prefix+prefix_n,sizeof(prefix)-(size_t)prefix_n,
                    ",\"raw_clock_anchor\":{\"boot_us\":\"%" PRIu64 "\",\"utc_us\":\"%" PRIu64 "\",\"uncert_us\":%s}",
                    a->queue_raw_boot[slot],a->queue_raw_utc[slot],uncertainty);
                if(part<0 || (size_t)part>=sizeof(prefix)-(size_t)prefix_n) return 0;
                prefix_n+=part;
            }
            if((size_t)prefix_n+1>=sizeof(prefix) || (size_t)k+(size_t)prefix_n+1>=sizeof(wire)-used) return 0;
            prefix[prefix_n++]=',';
            memmove(wire+used+prefix_n,wire+used+1,(size_t)k);
            memcpy(wire+used,prefix,(size_t)prefix_n);
            k+=prefix_n-1;
        }
        used += (size_t)k;
    }
    if (used + 3 >= sizeof(wire)) return 0;
    memcpy(wire + used, "]}\n", 4);
    return (int)(used + 3);
}
static int encode_bundle(app *a,unsigned head,unsigned n,losses loss) {
    unsigned i; size_t used=0; int bytes,prefix;
    prefix=snprintf(bundle_wire,sizeof(bundle_wire),"{\"schema\":\"mavlink_router_bundle_v1\",\"records\":[");
    if(prefix<0 || (size_t)prefix>=sizeof(bundle_wire)) return 0;
    used=(size_t)prefix;
    for(i=0;i<n;++i) {
        unsigned slot=(head+i)%QUEUE_CAP;
        bytes=a->queue_weather[slot] ? encode_weather(a,slot) : encode(a,slot,1,loss,a->queue_have_clock[slot] ? &a->queue_clock[slot] : NULL);
        if(bytes<=0) return 0;
        prefix=snprintf(bundle_wire+used,sizeof(bundle_wire)-used,"%s{\"kind\":\"%s\",\"payload\":",i ? "," : "",a->queue_weather[slot] ? "weather" : "telemetry");
        if(prefix<0 || (size_t)prefix>=sizeof(bundle_wire)-used) return 0;
        used+=(size_t)prefix;
        if((size_t)bytes+4>=sizeof(bundle_wire)-used) return 0;
        memcpy(bundle_wire+used,wire,(size_t)bytes); used+=(size_t)bytes;
        bundle_wire[used++]='}';
    }
    if(used+4>=sizeof(bundle_wire)) return 0;
    memcpy(bundle_wire+used,"]}\n",4); used+=3;
    memcpy(wire,bundle_wire,used+1);
    return (int)used;
}

static double precise_ms(void) {
#ifdef _WIN32
    LARGE_INTEGER ticks, frequency;
    (void)QueryPerformanceCounter(&ticks);
    (void)QueryPerformanceFrequency(&frequency);
    return (double)ticks.QuadPart * 1000.0 / (double)frequency.QuadPart;
#else
    struct timespec t;
    (void)clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1000000.0;
#endif
}
static void snapshot(void *context, tm_snapshot *out) {
    app *a = context;
    uint64_t now;
    lock(&a->mutex);
    now = now_ms();
    *out = a->diag;
    out->now_ms = now;
    out->eof = a->eof; out->failed = a->failed;
    out->rejected = a->loss.rejected;
    out->sequence_gaps = a->loss.sequence_gaps;
    out->raw_dropped = a->loss.raw_samples;
    out->queue_windows_dropped = a->loss.queue_windows;
    out->queue_samples_dropped = a->loss.queue_samples;
    out->queue_count = a->count; out->queue_capacity = QUEUE_CAP;
    out->current_raw = a->current.n ? a->current.raw_n : 0;
    out->current_observations = a->current.n;
    out->watermark_us = a->watermark_us;
    out->current_window_us = a->current.n ? a->current.window_us : 0;
    out->raw_capacity = TB_CAPACITY; out->send_capacity = SEND_WINDOWS;
    out->oldest_age_ms = a->count ? now - a->queued_at[a->head] : 0;
    out->last_boot_us = a->have_last ? a->last_t : 0;
    out->have_clock = tc_get_report(&a->clock, &out->clock);
    unlock(&a->mutex);
}
#ifdef _WIN32
static DWORD WINAPI monitor_worker(void *context)
#else
static void *monitor_worker(void *context)
#endif
{
    app *a = context;
    tm_run(a, snapshot, &a->config, a->config_path);
    return 0;
}
#ifdef _WIN32
static DWORD WINAPI sender(void *arg)
#else
static void *sender(void *arg)
#endif
{
    app *a = arg;
    http h;
    unsigned failures = 0;
    uint64_t interval = a->config.send_interval_ms;
    uint64_t next = now_ms() + interval;
    int dry = strcmp(a->url, "-") == 0;
    memset(&h, 0, sizeof(h));
    if (!dry && !http_open(&h, &a->config)) {
        fprintf(stderr, "HTTP setup failed\n");
        lock(&a->mutex); a->failed = 1; unlock(&a->mutex);
        http_close(&h);
        return 0;
    }
    lock(&a->mutex); a->ready = 1; unlock(&a->mutex);
    for (;;) {
        uint64_t now = now_ms(), completed;
        unsigned n, head;
        int is_weather, mixed=0;
        int eof, bytes, ok, have_clock, show_errors = 1;
        long status = 0;
        unsigned error = 0;
        double encode_start, encode_ms, post_start, post_ms;
        losses loss;
        tc_report report;
        if (now < next) { pause_ms((unsigned)(next - now > 100 ? 100 : next - now)); continue; }
        lock(&a->mutex);
        if (!a->test_mode && a->current.n && (a->eof || (a->live && now>=a->last_seal_ms && now-a->last_seal_ms>=interval))) seal(a);
        if(a->test_mode!=1 && a->weather_current.n && (a->eof || (now>=a->weather_last_seal_ms && now-a->weather_last_seal_ms>=interval))) seal_weather(a);
        head = a->head;
        is_weather=a->count && a->queue_weather[head];
        n = a->count < a->config.max_windows ? a->count : a->config.max_windows;
        /* One logical collection interval can hold both weather and legacy
         * chunks. Even max_windows=1 must drain those two types together. */
        if(a->live && a->test_mode!=1 && n==1 && a->count>=2 && SEND_WINDOWS>=2 && QUEUE_CAP>=2 &&
           a->queue_weather[head]!=a->queue_weather[(head+1)%QUEUE_CAP] &&
           !strcmp(a->queue_session[head],a->queue_session[(head+1)%QUEUE_CAP])) n=2;
        if(a->live && a->test_mode!=1 && n) {
            unsigned same=1;
            while(same<n && !strcmp(a->queue_session[(head+same)%QUEUE_CAP],a->queue_session[head])) {
                if(a->queue_weather[(head+same)%QUEUE_CAP]!=a->queue_weather[head]) mixed=1;
                ++same;
            }
            n=same;
        }
        if(is_weather && !mixed) n=1;
        eof = a->eof;
        loss = a->loss;
        if(a->live && n) {
            have_clock=a->queue_have_clock[head]; report=a->queue_clock[head];
        } else have_clock = tc_get_report(&a->clock, &report);
        unlock(&a->mutex);
        if (!n && eof) break;
        encode_start = precise_ms();
        bytes = n ? (mixed ? encode_bundle(a,head,n,loss) : is_weather ? encode_weather(a,head) : a->test_mode==1 ? encode_test(a,head,n) : encode(a, head, n, loss, have_clock ? &report : NULL)) : 0;
        encode_ms = precise_ms() - encode_start;
        if (n && !bytes) {
            fprintf(stderr, "JSON capacity exceeded\n");
            lock(&a->mutex); a->failed = 1; unlock(&a->mutex);
            break;
        }
        if (n) {
            lock(&a->mutex);
            a->diag.inflight_windows = n;
            ++a->diag.net_attempts;
            a->diag.last_attempt_ms = now_ms();
            a->diag.last_encode_ms = encode_ms;
            a->diag.net_sent_bytes += (uint64_t)bytes; /* attempted payload bytes */
            unlock(&a->mutex);
        }
        post_start = precise_ms();
        ok = !n || (dry ? (fwrite(wire, 1, (size_t)bytes, stdout) == (size_t)bytes && fflush(stdout) == 0)
                        : http_post(&h, &a->config, wire, (size_t)bytes, &status, &error));
        post_ms = precise_ms() - post_start;
        completed = now_ms();
        if (n) {
            lock(&a->mutex);
            a->diag.inflight_windows = 0;
            a->diag.http_status = status; a->diag.error_code = error;
            a->diag.last_rtt_ms = post_ms;
            if (ok) {
                unsigned released;
                for(released=0;released<n;++released) {
                    unsigned slot=(a->head+released)%QUEUE_CAP;
                    memset(&a->weather_queue[slot],0,sizeof(a->weather_queue[slot]));
                    memset(&a->queue[slot],0,sizeof(a->queue[slot]));
                    memset(&a->test_queue[slot],0,sizeof(a->test_queue[slot]));
                }
                a->head = (a->head + n) % QUEUE_CAP;
                a->count -= n;
                if (dry) ++a->diag.dry_releases;
                else ++a->diag.net_acks;
                a->diag.last_success_ms = completed;
                failures = 0;
            } else {
                ++failures;
                ++a->diag.net_failures;
            }
            a->diag.consecutive_failures = failures;
            show_errors = !a->monitor_enabled;
            unlock(&a->mutex);
            if (!ok) {
                if (show_errors)
                    fprintf(stderr, "Send failed; retaining %u window(s), attempt %u\n", n, failures);
                if (eof && failures >= 3) {
                    lock(&a->mutex); a->failed = 1; unlock(&a->mutex);
                    break;
                }
            }
        }
        /* Minimum configured interval between starts, including retries. */
        next = completed > now + interval ? completed : now + interval;
    }
    if (!dry) http_close(&h);
    return 0;
}
#ifndef TB_MONITOR_DEFAULT
#define TB_MONITOR_DEFAULT 0
#endif
#ifdef _WIN32
/* Windows CRT narrow argv uses the system code page. Decode Unicode arguments
 * once so configuration paths (including Korean directories) use UTF-8. */
static int utf8_args(int *argc, char ***argv, char *storage, size_t cap,
                     char **pointers, unsigned pointer_cap) {
    int n = 0, i;
    size_t used = 0;
    LPWSTR *wide = CommandLineToArgvW(GetCommandLineW(), &n);
    if (!wide || n < 1 || (unsigned)n >= pointer_cap) { if (wide) LocalFree(wide); return 0; }
    for (i = 0; i < n; ++i) {
        int len = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, NULL, 0, NULL, NULL);
        if (len <= 0 || (size_t)len > cap - used) { LocalFree(wide); return 0; }
        pointers[i] = storage + used;
        if (!WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, pointers[i], len, NULL, NULL)) {
            LocalFree(wide); return 0;
        }
        used += (size_t)len;
    }
    pointers[n] = NULL;
    LocalFree(wide); *argc = n; *argv = pointers;
    return 1;
}
#endif
int main(int argc, char **argv) {
    app *a = &state;
    char line[1024], error[160];
    unsigned i;
    int setup_failed = 0, monitor_started = 0, option_start, server_override = 0;
#ifdef _WIN32
    char argument_storage[32768];
    char *argument_pointers[32];
    HANDLE thread, monitor_thread = NULL;
    if (!utf8_args(&argc, &argv, argument_storage, sizeof(argument_storage),
                   argument_pointers, 32)) return 1;
#else
    pthread_t thread, monitor_thread;
#endif
    (void)setlocale(LC_NUMERIC, "C");
    tc_init(&a->clock);
    cfg_defaults(&a->config);
    a->config.max_windows = SEND_WINDOWS;
    a->monitor_enabled = TB_MONITOR_DEFAULT;
    a->config_path = default_config();
    if (argc == 2 && strcmp(argv[1], "--sizes") == 0) {
        printf("sample=%zu batch=%zu state=%zu wire=%zu bundle=%zu input_storage=%zu auxiliary_storage=%zu static_total=%zu\n",
               sizeof(tb_sample), sizeof(tb_batch), sizeof(state), sizeof(wire), sizeof(bundle_wire), ri_storage_bytes(), sizeof(default_config_path)+sizeof(stop_requested), sizeof(state) + sizeof(wire) + sizeof(bundle_wire) + ri_storage_bytes()+sizeof(default_config_path)+sizeof(stop_requested));
        return 0;
    }
    if(argc==1 || !strcmp(argv[1],"--interactive") || !strcmp(argv[1],"--headless") || !strcmp(argv[1],"--test-heartbeat") || !strcmp(argv[1],"--test-weather")) {
        if(config_exists(a->config_path)) {
            if(!cfg_load(a->config_path,&a->config,SEND_WINDOWS,error,sizeof(error))) {
                fprintf(stderr,"Configuration: %s\n",error); return 1;
            }
        } else {
            (void)cfg_set(&a->config,"url","-",error,sizeof(error));
            (void)cfg_set(&a->config,"stream","env1",error,sizeof(error));
            (void)cfg_set(&a->config,"session","auto",error,sizeof(error));
            if(!cfg_save(a->config_path,&a->config,SEND_WINDOWS,error,sizeof(error)))
                fprintf(stderr,"Could not save default settings: %s\n",error);
            fprintf(stderr,"No saved configuration: automatic MAVLink discovery and dry output. Configure server URL before production use.\n");
        }
        option_start=1;
    } else if (argc >= 3 && !strcmp(argv[1], "--config")) {
        a->config_path = argv[2];
        if (!cfg_load(a->config_path, &a->config, SEND_WINDOWS, error, sizeof(error))) {
            fprintf(stderr, "Configuration: %s\n", error); return 1;
        }
        option_start = 3;
    } else if (argc >= 7 && argv[1][0] != '\0') {
        const char *keys[] = {"url", "stream", "session", "origin_lat", "origin_lon", "origin_alt"};
        for (i = 0; i < 6; ++i) {
            if (!cfg_set(&a->config, keys[i], argv[i+1], error, sizeof(error))) {
                fprintf(stderr, "Configuration: %s\n", error); return 1;
            }
        }
        (void)strcpy(a->config.input_mode,"csv");
        option_start = 7;
    } else {
        fprintf(stderr, "Usage: mavlink_telemetry_data_router_headless URL|- STREAM SESSION LAT LON ALT [--summary] [--interactive]\n"
                        "   or: mavlink_telemetry_data_router_interactive --config telemetry.ini [--headless]\n"
                        "   test: --test-heartbeat | --test-weather [--server-url http://127.0.0.1:21000/ingest]\n"
                        "TUI: e edit, Enter validate, s save for restart; telemetry stays in memory.\n");
        return 1;
    }
    for (i = (unsigned)option_start; i < (unsigned)argc; ++i) {
        if (!strcmp(argv[i],"--test-heartbeat")) a->test_mode=1;
        else if(!strcmp(argv[i],"--test-weather")) a->test_mode=2;
        else if(!strcmp(argv[i],"--server-url") && i+1<(unsigned)argc) {
            if(!cfg_set(&a->config,"url",argv[++i],error,sizeof(error))) { fprintf(stderr,"Configuration: %s\n",error); return 1; }
            server_override=1;
        }
        else if (!strcmp(argv[i], "--summary")) a->config.summary = 1;
        else if (!strcmp(argv[i], "--interactive") || !strcmp(argv[i], "--monitor")) a->monitor_enabled = 1;
        else if (!strcmp(argv[i], "--headless") || !strcmp(argv[i], "--no-monitor")) a->monitor_enabled = 0;
        else { fprintf(stderr, "Unknown option\n"); return 1; }
    }
    if(a->test_mode && !server_override)
        (void)cfg_set(&a->config,"url","http://127.0.0.1:21000/ingest",error,sizeof(error));
    if(a->test_mode && strcmp(a->config.input_mode,"mavlink")) { fprintf(stderr,"Sensor test requires MAVLink input\n"); return 1; }
    if(a->test_mode) {
        char safe_url[2048]; cfg_safe_url(a->config.url,safe_url,sizeof(safe_url));
        fprintf(stderr,"TEST MODE: %s; HTTP/TCP server %s\n",a->test_mode==1 ? "heartbeat and coordinates only" : "hygrometer temperature/humidity with acquisition time, position and statistics",safe_url);
    }
    if (!cfg_validate(&a->config, SEND_WINDOWS, error, sizeof(error))) {
        fprintf(stderr, "Configuration: %s\n", error); return 1;
    }
    a->live=!strcmp(a->config.input_mode,"mavlink");
    if(a->live && !run_nonce(&a->run_nonce)) { fprintf(stderr,"Could not generate unique router run ID\n"); return 1; }
    if(a->live) input_signals();
#ifndef _WIN32
    /* Initialize before the input and sender use libcurl concurrently. */
    if(curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK) return 1;
#endif
    a->last_seal_ms=now_ms();
    a->weather_last_seal_ms=now_ms();
    a->url = a->config.url; a->stream = a->config.stream;
    a->session = a->config.session; a->summary = a->config.summary;
    memcpy(a->origin, a->config.origin, sizeof(a->origin));
    a->origin_known=!a->config.origin_auto;
    a->diag.started_ms = now_ms(); a->diag.dry_run = !strcmp(a->url, "-");
    a->diag.test_mode=a->test_mode;
#ifdef _WIN32
    InitializeCriticalSection(&a->mutex);
    thread = CreateThread(NULL, 0, sender, a, 0, NULL);
    if (!thread) { DeleteCriticalSection(&a->mutex); return 1; }
    if (a->monitor_enabled) {
        monitor_thread = CreateThread(NULL, 0, monitor_worker, a, 0, NULL);
        monitor_started = monitor_thread != NULL;
    }
#else
    if (pthread_mutex_init(&a->mutex, NULL)) return 1;
    if (pthread_create(&thread, NULL, sender, a)) { (void)pthread_mutex_destroy(&a->mutex); return 1; }
    if (a->monitor_enabled) monitor_started = pthread_create(&monitor_thread, NULL, monitor_worker, a) == 0;
#endif
    if (a->monitor_enabled && !monitor_started) {
        fprintf(stderr, "Monitor could not start; receiver continues\n");
        lock(&a->mutex); a->monitor_enabled = 0; unlock(&a->mutex);
    }
    /* Do not block on idle stdin until the sender has validated its setup. */
    for (;;) {
        int ready;
        lock(&a->mutex); ready = a->ready; setup_failed = a->failed; unlock(&a->mutex);
        if (ready || setup_failed) break;
        pause_ms(10);
    }
    if (setup_failed) goto finish;
    if(a->live) {
        (void)ri_run(&a->config,mavlink_event,input_status,input_stop,a);
    } else while (fgets(line, sizeof(line), stdin)) {
        tb_sample sample;
        char *p = line;
        int bad = 0;
        size_t input_len = strlen(line);
        if (!strchr(line, '\n') && !feof(stdin)) {
            int c;
            while ((c = getchar()) != '\n' && c != EOF) ++input_len;
            if (c == '\n') ++input_len;
            bad = 1;
        }
        lock(&a->mutex);
        a->diag.input_bytes += input_len; ++a->diag.input_lines;
        a->diag.last_receipt_ms = now_ms();
        unlock(&a->mutex);
        while (isspace((unsigned char)*p)) ++p;
        if (!*p || *p == '#') continue;
        if (!bad && *p == '@') {
            if (!control(a, p)) { lock(&a->mutex); ++a->loss.rejected; unlock(&a->mutex); }
        } else if (!bad && parse(p, &sample)) ingest(a, &sample);
        else { lock(&a->mutex); ++a->loss.rejected; unlock(&a->mutex); }
        lock(&a->mutex); bad = a->failed; unlock(&a->mutex);
        if (bad) break;
    }
finish:
    lock(&a->mutex);
    if (ferror(stdin)) a->failed = 1;
    a->eof = 1;
    unlock(&a->mutex);
#ifdef _WIN32
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
#else
    (void)pthread_join(thread, NULL);
#endif
    lock(&a->mutex); a->diag.stop = 1; unlock(&a->mutex);
    if (monitor_started) {
#ifdef _WIN32
        WaitForSingleObject(monitor_thread, INFINITE); CloseHandle(monitor_thread);
#else
        (void)pthread_join(monitor_thread, NULL);
#endif
    }
#ifdef _WIN32
    DeleteCriticalSection(&a->mutex);
#else
    (void)pthread_mutex_destroy(&a->mutex);
#endif
#ifndef _WIN32
    curl_global_cleanup();
#endif
    fprintf(stderr, "rejected=%" PRIu64 " sequence_gaps=%" PRIu64 " raw_dropped=%" PRIu64
            " queue_windows_dropped=%" PRIu64 " queue_samples_dropped=%" PRIu64 " pending=%u\n",
            a->loss.rejected, a->loss.sequence_gaps, a->loss.raw_samples,
            a->loss.queue_windows, a->loss.queue_samples, a->count);
    return a->failed || a->loss.rejected || a->loss.sequence_gaps || a->loss.raw_samples || a->loss.queue_windows ? 2 : 0;
}
