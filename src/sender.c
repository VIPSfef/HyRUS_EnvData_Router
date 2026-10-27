/* HTTP ACK contract: 204 means every request batch was committed by the server. */
/* MAVLink/CSV -> immediate UDP records and a byte-bounded HTTP memory FIFO. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "telemetry.h"
#include "time_sync.h"
#include "config.h"
#include "tb_version.h"
#include "monitor.h"
#include "router_input.h"
#include "spatial.h"
#include "status_report.h"
#include "udp_backup.h"
#include "memory_queue.h"
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
typedef HANDLE worker_t;
typedef LPTHREAD_START_ROUTINE worker_fn;
static void lock(mutex_t *m) { EnterCriticalSection(m); }
static void unlock(mutex_t *m) { LeaveCriticalSection(m); }
static int mutex_init(mutex_t *m) { InitializeCriticalSection(m); return 1; }
static void mutex_destroy(mutex_t *m) { DeleteCriticalSection(m); }
static int worker_start(worker_t *worker, worker_fn function, void *context) {
    *worker=CreateThread(NULL,0,function,context,0,NULL);
    return *worker!=NULL;
}
static void worker_join(worker_t worker) {
    (void)WaitForSingleObject(worker,INFINITE); (void)CloseHandle(worker);
}
static uint64_t now_ms(void) { return GetTickCount64(); }
static void pause_ms(unsigned ms) { Sleep(ms); }
#else
#include <pthread.h>
#include <time.h>
#include <curl/curl.h>
#include <sys/random.h>
#include <unistd.h>
typedef pthread_mutex_t mutex_t;
typedef pthread_t worker_t;
typedef void *(*worker_fn)(void *);
static void lock(mutex_t *m) { (void)pthread_mutex_lock(m); }
static void unlock(mutex_t *m) { (void)pthread_mutex_unlock(m); }
static int mutex_init(mutex_t *m) { return pthread_mutex_init(m,NULL)==0; }
static void mutex_destroy(mutex_t *m) { (void)pthread_mutex_destroy(m); }
static int worker_start(worker_t *worker, worker_fn function, void *context) {
    return pthread_create(worker,NULL,function,context)==0;
}
static void worker_join(worker_t worker) { (void)pthread_join(worker,NULL); }
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

#ifndef TB_SEND_WINDOWS
#define TB_SEND_WINDOWS 4u
#endif
#if TB_SEND_WINDOWS < 1
#error Invalid send window capacity
#endif
#define SEND_WINDOWS TB_SEND_WINDOWS
/* Includes worst-width uint64, subnormal doubles, keys, units and error fields. */
#define BATCH_JSON_CAP (TB_CAPACITY * 512u + TB_MAX_KEYS * 768u + 2048u)
#define WIRE_CAP (SEND_WINDOWS * BATCH_JSON_CAP + 4096u)
enum { FINAL_SEND_ATTEMPTS=3, BACKUP_TICK_MS=10, BACKUP_REOPEN_MS=2000,
       HTTP_BACKOFF_INITIAL_MS=1000, HTTP_BACKOFF_MAX_MS=30000 };
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
typedef enum { RECORD_TELEMETRY, RECORD_WEATHER, RECORD_TEST } record_kind;
typedef struct {
    double completed_ms, next_ms;
    unsigned backoff_ms;
} http_pacing;
static double precise_ms(void);
/* One gate covers status, measurements, retries and shutdown/restart drains.
 * Completion-based spacing is conservative even after a slow request. A 403
 * can mean permissions, credentials or gateway policy; it is not classified
 * as proof of rate limiting. Never spend missed slots in a catch-up burst. */
static void http_pacing_complete(http_pacing *p,unsigned minimum_ms,long status,double completed) {
    unsigned delay=minimum_ms;
    if(status==204) p->backoff_ms=0;
    else if(status==401 || status==403 || status==429) {
        if(!p->backoff_ms) p->backoff_ms=HTTP_BACKOFF_INITIAL_MS;
        else if(p->backoff_ms>=HTTP_BACKOFF_MAX_MS/2) p->backoff_ms=HTTP_BACKOFF_MAX_MS;
        else p->backoff_ms*=2;
    }
    if(p->backoff_ms>delay) delay=p->backoff_ms;
    p->completed_ms=completed;
    p->next_ms=completed+delay;
}
typedef struct {
    char token[CFG_AUTH_TOKEN_BYTES];
    int valid;
} http_auth;
static http_auth resolve_http_auth(const tb_config *config) {
    http_auth auth={{0},1};
    const char *token=cfg_auth_token(config);
    size_t n=token ? strlen(token) : 0;
    auth.valid=n<=CFG_AUTH_TOKEN_MAX && (!token || !strpbrk(token,"\r\n"));
    if(auth.valid && n) memcpy(auth.token,token,n+1);
    return auth;
}
typedef struct {
    record_kind kind;
    union {
        tb_batch telemetry;
        struct { weather_batch weather; sp_batch spatial; } w;
        test_record test;
    } data;
    uint64_t queued_at, batch_id;
    char session[65];
    uint8_t system, component;
    uint32_t generation;
    double origin[3];
    tc_report clock;
    int have_clock, origin_known;
    uint64_t raw_boot, raw_utc;
    uint32_t raw_uncert;
} send_record;
typedef struct {
    mutex_t mutex;
    tb_batch current;
    weather_batch weather_current;
    send_record scratch;
    mq_queue queue;
    sp_state spatial;
    ub_sender *backup_socket;
    int backup_stop;
    uint64_t weather_last_seal_ms;
    uint64_t last_t, watermark_us, sealed_window;
    metric_state metric[TB_MAX_KEYS];
    unsigned metric_n;
    tc_clock clock;
    int have_last, have_sealed, eof, failed, ready, summary;
    losses loss;
    const char *url, *stream, *session;
    double origin[3];
    tb_config config;
    tb_config restart_config;
    /* Mutable only under mutex; the remaining active configuration is immutable. */
    http_auth auth;
    /* Sender-owned while running; retained across in-process restarts. */
    http_pacing pacing;
    int restart_requested, restart_blocked;
    unsigned restart_attempt;
    tm_snapshot diag;
    const char *config_path;
    int monitor_enabled, live, test_mode;
    uint64_t test_next_seq, test_last_heartbeat, test_last_enqueue_ms, test_epoch_start_ms;
    uint64_t status_next_seq;
    char active_session[65];
    uint8_t source_system, source_component;
    uint32_t route_generation;
    uint64_t run_nonce, next_batch_id, last_seal_ms;
    int origin_known;
    uint64_t raw_clock_boot,raw_clock_utc;
    uint32_t raw_clock_uncert;
} app;
static app state; /* Fixed collection scratch; pending JSON is bounded heap storage. */
static char wire[WIRE_CAP];
static char backup_encoded[WIRE_CAP]; /* producer-owned under app mutex */
static char backup_output[WIRE_CAP]; /* UDP worker-owned until joined */
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
static int shutdown_requested(void) {
#ifdef _WIN32
    return InterlockedCompareExchange(&stop_requested,0,0)!=0;
#else
    return stop_requested!=0;
#endif
}
/* Caller holds mutex. Spatial open bins also need their final boundary sealed. */
static int unsealed_data(const app *a) {
    return a->current.n || a->weather_current.n || a->spatial.completed_n ||
           (a->config.spatial_enabled && a->spatial.report.open_n);
}
static int input_stop(void *context) {
    app *a=context;
    for(;;) {
        int stop,wait=0;
        lock(&a->mutex);
        stop=a->failed || shutdown_requested();
        if(!stop && a->restart_requested) {
            if(a->restart_blocked) {
                if(unsealed_data(a)) {
                    /* Cancel applying the draft, but do not resume collection
                     * into a full memory buffer while old partials remain unsealed. */
                    (void)snprintf(a->diag.restart_notice,sizeof(a->diag.restart_notice),
                                   "Restart blocked: pending data retained; input paused until queue space is available.");
                    wait=1;
                } else {
                    a->restart_requested=0; a->restart_blocked=0;
                    a->auth=resolve_http_auth(&a->config);
                    a->diag.restarting=0;
                    (void)snprintf(a->diag.restart_notice,sizeof(a->diag.restart_notice),
                                   "Restart blocked: pending data retained; previous settings resumed.");
                }
            } else if(a->queue.count || unsealed_data(a) || a->diag.inflight_windows) wait=1;
            else stop=1;
        }
        unlock(&a->mutex);
        if(!wait) return stop;
        /* ri_run is paused here before decoder finish/reset or port closure.
         * Sender and monitor remain responsive; no new sample can force a
         * partial window out of a full queue while the old endpoint retries. */
        pause_ms(10);
    }
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
/* Both transports receive one sealed record with identical identity and data.
 * HTTP retains compact immutable JSON; UDP attempts a nonblocking send now.
 * The scratch record and publication are protected by the app mutex. */
static int publish_record(app *a,unsigned samples);
static send_record *prepare_record(app *a,record_kind kind) {
    send_record *r=&a->scratch;
    memset(r,0,sizeof(*r)); r->kind=kind;
    r->queued_at=now_ms(); r->batch_id=++a->next_batch_id;
    (void)snprintf(r->session,sizeof(r->session),"%s",a->live ? a->active_session : a->session);
    memcpy(r->origin,a->origin,sizeof(r->origin)); r->origin_known=a->origin_known;
    r->system=a->source_system; r->component=a->source_component; r->generation=a->route_generation;
    r->have_clock=tc_get_report(&a->clock,&r->clock);
    r->raw_boot=a->raw_clock_boot; r->raw_utc=a->raw_clock_utc; r->raw_uncert=a->raw_clock_uncert;
    return r;
}
static void seal(app *a) {
    send_record *r;
    if(!a->current.n) return;
    r=prepare_record(a,RECORD_TELEMETRY); r->data.telemetry=a->current;
    if(!publish_record(a,a->current.n) && a->restart_requested) return;
    a->sealed_window=a->current.window_us; a->have_sealed=1;
    a->current.n=0; a->last_seal_ms=now_ms();
}
static void seal_weather(app *a) {
    send_record *r;
    if(!a->weather_current.n && !a->spatial.completed_n) return;
    r=prepare_record(a,RECORD_WEATHER); r->data.w.weather=a->weather_current;
    sp_prepare_snapshot(&a->spatial,&r->data.w.spatial);
    if(!publish_record(a,a->weather_current.n) && a->restart_requested) return;
    sp_commit_snapshot(&a->spatial);
    weather_batch_init(&a->weather_current,now_ms()); a->weather_last_seal_ms=now_ms();
}
/* Called by HTTP and the independent UDP clock. A single locked path advances
 * collection windows even while an HTTP request waits for its response. */
static void seal_due(app *a,uint64_t now,int draining,int restarting) {
    uint64_t interval=a->config.send_interval_ms;
    if(!a->test_mode && a->current.n && (!restarting || a->queue.limit-a->queue.bytes>=BATCH_JSON_CAP+sizeof(mq_record)) &&
       (draining || (a->live && now>=a->last_seal_ms && now-a->last_seal_ms>=interval))) seal(a);
    if(a->test_mode!=1 && (a->weather_current.n || a->spatial.completed_n) &&
       (!restarting || a->queue.limit-a->queue.bytes>=BATCH_JSON_CAP+sizeof(mq_record)) &&
       (draining || (now>=a->weather_last_seal_ms && now-a->weather_last_seal_ms>=interval))) seal_weather(a);
}
static void ingest_weather(app *a,const mi_event *e) {
    const weather_sample *s=&e->weather; unsigned k;
    lock(&a->mutex);
    a->source_system=e->sysid; a->source_component=e->compid;
    if(!a->weather_current.n) weather_batch_init(&a->weather_current,now_ms());
    if(weather_batch_add(&a->weather_current,s)) {
        if(a->config.spatial_enabled)
            (void)sp_add(&a->spatial,s,((uint64_t)a->route_generation<<32)|s->navigation_segment,s->has_velocity);
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
            key->seq=s->seq; key->value=s->value; key->have_value=1; key->last_receipt_ms=now_ms();
        }
        if(s->has_position) {
            a->diag.have_position=1; memcpy(a->diag.p,s->p,sizeof(s->p)); memcpy(a->diag.v,s->v,sizeof(s->v));
            a->diag.time_uncert_us=s->time_uncert_us; a->diag.pos_error_m=s->pos_error_m;
            a->diag.nav_age_us=UINT32_MAX;
        }
        if(a->config.udp_backup_enabled) seal_weather(a);
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
            if(a->live && a->config.udp_backup_enabled) seal(a);
        } else ++a->loss.rejected;
    }
    unlock(&a->mutex);
}
/* Only a selected lane may replace device history. Discovery keeps the last
 * real receipts so reports can distinguish link loss from never connecting.
 * A null report explicitly starts a new selected transport epoch. */
static void device_status(tm_snapshot *out,const mi_report *report) {
    const mi_device *selected=report ? mi_selected_device(report) : NULL;
    unsigned i;
    out->selected_system=report ? report->selected_system : 0;
    out->selected_component=report ? report->selected_component : 0;
    out->have_heartbeat=selected && selected->have_heartbeat;
    out->have_data=selected && selected->have_data;
    out->heartbeat_received_ms=selected ? selected->last_heartbeat_ms : 0;
    out->data_received_ms=selected ? selected->last_data_ms : 0;
    out->received_packets=selected ? selected->packets : 0;
    out->estimated_sequence_gaps=selected ? selected->sequence_gaps : 0;
    out->duplicate_packets=selected ? selected->duplicates : 0;
    out->device_n=report ? (report->devices<TM_MAX_DEVICES ? report->devices : TM_MAX_DEVICES) : 0;
    if(!report) out->last_receipt_ms=0;
    for(i=0;i<out->device_n;++i) {
        const mi_device *d=&report->device[i]; tm_device *device=&out->devices[i];
        device->sysid=d->sysid; device->compid=d->compid; device->type=d->type; device->autopilot=d->autopilot;
        device->base_mode=d->base_mode; device->system_status=d->system_status;
        device->have_heartbeat=d->have_heartbeat; device->have_data=d->have_data;
        device->last_heartbeat_ms=d->last_heartbeat_ms; device->last_data_ms=d->last_data_ms;
        device->packets=d->packets; device->sequence_gaps=d->sequence_gaps; device->duplicates=d->duplicates;
        device->custom_mode=d->custom_mode; device->last_seen_ms=d->last_seen_ms;
        if(d->last_seen_ms>out->last_receipt_ms) out->last_receipt_ms=d->last_seen_ms;
    }
}
static void format_session(app *a) {
    (void)snprintf(a->active_session,sizeof(a->active_session),"%.28s.%016" PRIx64 ".%u",
                   a->session,a->run_nonce,a->route_generation);
}
static void next_session(app *a,const mi_event *event) {
    tm_snapshot *diag=&a->diag;
    /* Caller holds the same mutex used by status snapshots. Clear location
     * before publishing a new session, including the interval before the next
     * receiver status callback; an old source's coordinate cannot cross epochs. */
    diag->have_position=diag->have_geodetic=diag->have_last_geodetic=0;
    diag->have_fix_age=diag->have_position_error=diag->navigation_invalid=0;
    diag->position_kind=TM_POSITION_UNKNOWN; diag->gps_fix=0;
    diag->navigation_received_ms=diag->last_fix_age_ms=0;
    diag->latitude_deg=diag->longitude_deg=diag->altitude_msl_m=NAN;
    diag->estimated_error_m=NAN;
    ++a->route_generation;
    format_session(a);
    /* The router's selection reset has epoch zero. Decoder clock resets use
     * positive epochs and keep receipt history from this same byte source. */
    if(!event->epoch) device_status(&a->diag,NULL);
}
static void mavlink_event(void *context,const mi_event *event) {
    app *a=context;
    if(a->test_mode==1) {
        if(event->kind==MI_RESET) {
            lock(&a->mutex);
            next_session(a,event);
            a->test_epoch_start_ms=now_ms();
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
        sp_break(&a->spatial,SP_FLAG_CONTEXT_CHANGE);
        seal(a); seal_weather(a);
        a->origin_known=!a->config.origin_auto;
        a->metric_n=0; a->have_last=a->have_sealed=0; a->watermark_us=0;
        a->source_system=event->sysid; a->source_component=event->compid;
        a->diag.key_n=0; memset(a->diag.keys,0,sizeof(a->diag.keys));
        tc_init(&a->clock);
        a->raw_clock_boot=a->raw_clock_utc=0; a->raw_clock_uncert=UINT32_MAX;
        next_session(a,event);
    } else if(event->kind==MI_ORIGIN) {
        sp_break(&a->spatial,SP_FLAG_CONTEXT_CHANGE);
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
    uint64_t now=now_ms();
    const mi_device *device=mi_selected_device(&r->mav);
    test_record *record;
    if(strcmp(r->status,"LOCKED")) return;
    if(!device || !device->have_heartbeat || device->last_heartbeat_ms<=a->test_epoch_start_ms ||
       device->last_heartbeat_ms==a->test_last_heartbeat ||
       now-device->last_heartbeat_ms>=a->config.link_timeout_ms ||
       (a->test_last_enqueue_ms && now-a->test_last_enqueue_ms<a->config.send_interval_ms)) return;
    a->test_last_heartbeat=device->last_heartbeat_ms;
    a->test_last_enqueue_ms=now;
    record=&prepare_record(a,RECORD_TEST)->data.test;
    record->seq=++a->test_next_seq; record->generation=a->route_generation;
    record->captured_ms=now; record->heartbeat=*device;
    (void)snprintf(record->session,sizeof(record->session),"%s",a->active_session);
    (void)snprintf(record->source,sizeof(record->source),"%s",r->source);
    record->have_navigation=r->mav.have_navigation && now>=r->mav.navigation_received_ms &&
        now-r->mav.navigation_received_ms<a->config.link_timeout_ms;
    record->lat=r->mav.lat; record->lon=r->mav.lon; record->altitude_msl_m=r->mav.altitude_msl_m;
    record->navigation_received_ms=r->mav.navigation_received_ms;
    record->navigation_boot_us=r->mav.navigation_boot_us;
    publish_record(a,1); ++a->diag.accepted;
    a->diag.last_receipt_ms=now;
}
static void input_status(void *context,const ri_report *report) {
    app *a=context;
    lock(&a->mutex);
    (void)snprintf(a->diag.active_source,sizeof(a->diag.active_source),"%s",report->source);
    (void)snprintf(a->diag.source_status,sizeof(a->diag.source_status),"%s",report->status);
    (void)snprintf(a->diag.source_error,sizeof(a->diag.source_error),"%s",report->error);
    a->diag.serial_baud=report->serial_baud;
    (void)snprintf(a->diag.qgc_status,sizeof(a->diag.qgc_status),"%s",report->qgc_status);
    (void)snprintf(a->diag.qgc_error,sizeof(a->diag.qgc_error),"%s",report->qgc.error);
    a->diag.qgc_tx_frames=report->qgc.tx_frames; a->diag.qgc_tx_bytes=report->qgc.tx_bytes;
    a->diag.qgc_rx_frames=report->qgc.rx_frames; a->diag.qgc_rx_bytes=report->qgc.rx_bytes;
    a->diag.qgc_dropped=report->qgc.dropped; a->diag.qgc_errors=report->qgc.errors;
    a->diag.qgc_local_port=report->qgc.local_port;
    a->diag.rates=report->rates;
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
    a->diag.gps_fix=report->mav.gps_fix;
    a->diag.battery=report->mav.battery;
    if(report->source_selected) device_status(&a->diag,&report->mav);
    {
        const mi_report *nav=&report->mav;
        uint64_t at=now_ms();
        int fresh=report->source_selected && nav->have_navigation && !strcmp(report->status,"LOCKED") &&
            at>=nav->navigation_received_ms && at-nav->navigation_received_ms<=a->config.link_timeout_ms;
        a->diag.last_boot_us=nav->boot_us;
        a->diag.have_geodetic=fresh && nav->position_kind!=WEATHER_POSITION_UNKNOWN &&
            isfinite(nav->lat) && fabs(nav->lat)<=90 && isfinite(nav->lon) && fabs(nav->lon)<=180 &&
            isfinite(nav->altitude_msl_m);
        if(report->source_selected) {
            a->diag.have_last_geodetic=nav->have_last_geodetic &&
                isfinite(nav->lat) && fabs(nav->lat)<=90 && isfinite(nav->lon) && fabs(nav->lon)<=180 &&
                isfinite(nav->altitude_msl_m);
            a->diag.position_kind=a->diag.have_last_geodetic ?
                (nav->last_position_kind==WEATHER_POSITION_GPS ? TM_POSITION_GPS : TM_POSITION_ESTGPS) :
                (nav->have_gps_anchor ? TM_POSITION_UNKNOWN : TM_POSITION_NO_ANCHOR);
            a->diag.latitude_deg=nav->lat; a->diag.longitude_deg=nav->lon;
            a->diag.altitude_msl_m=nav->altitude_msl_m;
            a->diag.navigation_received_ms=nav->navigation_received_ms;
            a->diag.navigation_invalid=nav->navigation_invalid;
        }
        a->diag.have_fix_age=nav->have_gps_anchor && nav->boot_us>=nav->last_fix_boot_us;
        a->diag.last_fix_age_ms=a->diag.have_fix_age ? (nav->boot_us-nav->last_fix_boot_us)/1000 : 0;
        a->diag.have_position_error=a->diag.have_geodetic && isfinite(nav->navigation_error_m) && nav->navigation_error_m>=0;
        a->diag.estimated_error_m=a->diag.have_position_error ? nav->navigation_error_m : NAN;
        if(!a->diag.have_geodetic) a->diag.have_position=0;
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
    h->session = WinHttpOpen(TB_USER_AGENT_W, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h->session) return 0;
    if (!WinHttpSetTimeouts(h->session, (int)config->connect_timeout_ms,
        (int)config->connect_timeout_ms, (int)config->io_timeout_ms, (int)config->io_timeout_ms)) return 0;
    h->connection = WinHttpConnect(h->session, host, c.nPort, 0);
    return h->connection != NULL;
}
static int http_post(http *h, const char *authorization, const char *body, size_t n,
                     long *status_out, unsigned *error_out) {
    DWORD status = 0, len = sizeof(status), redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    HINTERNET r = WinHttpOpenRequest(h->connection, L"POST", h->route, NULL,
                          WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, h->flags);
    int ok = 0;
    *status_out = 0; *error_out = 0;
    if (!r) { *error_out = (unsigned)GetLastError(); return 0; }
    if (!WinHttpSetOption(r, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects)) ||
        !WinHttpAddRequestHeaders(r, L"Content-Type: application/json\r\n", (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD)) goto done;
    if (*authorization) {
        wchar_t wheader[CFG_AUTH_TOKEN_BYTES+sizeof("Authorization: Bearer ")];
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, authorization, -1, wheader,
                                (int)(sizeof(wheader)/sizeof(wheader[0]))) ||
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
typedef struct { CURL *easy; int global_ready; } http;
static size_t discard(char *p, size_t size, size_t n, void *u) {
    (void)p; (void)u; return size * n;
}
static int http_open(http *h, const tb_config *config) {
    const char *url = config->url;
    memset(h, 0, sizeof(*h));
    if (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) return 0;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return 0;
    h->global_ready = 1;
    h->easy = curl_easy_init();
    if (!h->easy) return 0;
    curl_easy_setopt(h->easy, CURLOPT_URL, url);
    curl_easy_setopt(h->easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h->easy, CURLOPT_CONNECTTIMEOUT_MS, (long)config->connect_timeout_ms);
    curl_easy_setopt(h->easy, CURLOPT_TIMEOUT_MS, (long)config->connect_timeout_ms + 2L * config->io_timeout_ms);
    curl_easy_setopt(h->easy, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(h->easy, CURLOPT_WRITEFUNCTION, discard);
    return 1;
}
static int http_post(http *h, const char *authorization, const char *body, size_t n,
                     long *status_out, unsigned *error_out) {
    long status = 0;
    CURLcode result;
    struct curl_slist *headers=curl_slist_append(NULL,"Content-Type: application/json");
    *status_out=0; *error_out=CURLE_OUT_OF_MEMORY;
    if(!headers) return 0;
    if(*authorization) {
        struct curl_slist *list=curl_slist_append(headers,authorization);
        if(!list) { curl_slist_free_all(headers); return 0; }
        headers=list;
    }
    curl_easy_setopt(h->easy, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(h->easy, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(h->easy, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)n);
    result = curl_easy_perform(h->easy);
    (void)curl_easy_getinfo(h->easy, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_setopt(h->easy, CURLOPT_HTTPHEADER, NULL);
    curl_slist_free_all(headers);
    *status_out = status; *error_out = (unsigned)result;
    return result == CURLE_OK && status == 204;
}
static void http_close(http *h) {
    if (h->easy) curl_easy_cleanup(h->easy);
    if (h->global_ready) curl_global_cleanup();
}
#endif

/* Capture once per request so a same-endpoint credential correction can drain
 * retained measurements. Credentials never enter the JSON or UDP encoders. */
static int http_post_snapshot(app *a,http *h,const char *body,size_t n,
                              long *status,unsigned *error) {
    http_auth auth;
    int ok;
    char header[CFG_AUTH_TOKEN_BYTES+sizeof("Authorization: Bearer ")];
    lock(&a->mutex); auth=a->auth; unlock(&a->mutex);
    header[0]=0;
    if(auth.token[0]) (void)snprintf(header,sizeof(header),"Authorization: Bearer %s",auth.token);
    if(!auth.valid) { *status=0; *error=EINVAL; ok=0; }
    else ok=http_post(h,header,body,n,status,error);
    http_pacing_complete(&a->pacing,a->config.http_min_interval_ms,*status,precise_ms());
    return ok;
}

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

static int json_append(char *output,size_t capacity,size_t *used,const char *format,...) {
    va_list args; int n;
    va_start(args,format);
    n=vsnprintf(output+*used,capacity-*used,format,args);
    va_end(args);
    if(n<0 || (size_t)n>=capacity-*used) return 0;
    *used+=(size_t)n; return 1;
}
static int json_quote(char *output,size_t capacity,size_t *used,const char *value) {
    const unsigned char *p=(const unsigned char *)value;
    if(!json_append(output,capacity,used,"\"")) return 0;
    for(;*p;++p) {
        if(*p<32 || *p=='\"' || *p=='\\') {
            if(!json_append(output,capacity,used,"\\u%04x",(unsigned)*p)) return 0;
        } else if(!json_append(output,capacity,used,"%c",*p)) return 0;
    }
    return json_append(output,capacity,used,"\"");
}
static int encode_test(app *a,const send_record *record,char *output,size_t capacity) {
    size_t used=0;
    if(!json_append(output,capacity,&used,"{\"schema\":\"mavlink_router_test_v1\",\"stream\":") ||
       !json_quote(output,capacity,&used,a->stream) || !json_append(output,capacity,&used,",\"records\":[")) return 0;
    {
        const test_record *r=&record->data.test;
        const mi_device *d=&r->heartbeat;
        if(!json_append(output,capacity,&used,"{\"seq\":%" PRIu64 ",\"source_generation\":%" PRIu64 ",\"session\":",r->seq,r->generation) ||
           !json_quote(output,capacity,&used,r->session) || !json_append(output,capacity,&used,",\"source\":") || !json_quote(output,capacity,&used,r->source) ||
           !json_append(output,capacity,&used,",\"heartbeat\":{\"sysid\":%u,\"compid\":%u,\"type\":%u,\"autopilot\":%u,"
             "\"base_mode\":%u,\"custom_mode\":%u,\"system_status\":%u,\"received_monotonic_ms\":%" PRIu64 "},\"position\":",
             (unsigned)d->sysid,(unsigned)d->compid,(unsigned)d->type,(unsigned)d->autopilot,
             (unsigned)d->base_mode,(unsigned)d->custom_mode,(unsigned)d->system_status,d->last_heartbeat_ms)) return 0;
        if(r->have_navigation) {
            if(!json_append(output,capacity,&used,"{\"lat_deg\":%.17g,\"lon_deg\":%.17g,\"alt_msl_m\":%.17g,"
                "\"boot_ms\":%" PRIu64 ",\"received_monotonic_ms\":%" PRIu64 "},\"coordinate_age_ms\":%" PRIu64 "}",
                r->lat,r->lon,r->altitude_msl_m,r->navigation_boot_us/1000,r->navigation_received_ms,
                r->captured_ms>=r->navigation_received_ms ? r->captured_ms-r->navigation_received_ms : 0)) return 0;
        } else if(!json_append(output,capacity,&used,"null,\"coordinate_age_ms\":null}")) return 0;
    }
    return json_append(output,capacity,&used,"]}\n") ? (int)used : 0;
}
static int encode_weather(app *a,send_record *record,char *output,size_t capacity) {
    size_t used=0; int n; const double *origin=record->origin;
    if(!json_append(output,capacity,&used,"{\"schema\":\"mavlink_weather_v1\",\"stream\":") || !json_quote(output,capacity,&used,a->stream) ||
       !json_append(output,capacity,&used,",\"records\":[{\"seq\":%" PRIu64 ",\"generation\":%u,\"session\":",record->batch_id,record->generation) ||
       !json_quote(output,capacity,&used,record->session) ||
       !json_append(output,capacity,&used,",\"source\":{\"sysid\":%u,\"compid\":%u},\"frame\":\"ENU\",\"origin_wgs84\":[%.17g,%.17g,%.17g],"
         "\"time_basis\":\"pixhawk_boot_us\",\"host_time_basis\":\"collector_monotonic_ms\",\"clock_model\":",
         (unsigned)record->system,(unsigned)record->component,origin[0],origin[1],origin[2])) return 0;
    if(!record->origin_known) {
        char *start=strstr(output,"\"origin_wgs84\":["), *end;
        if(start && (end=strchr(start,']'))!=NULL) {
            start+=15; /* retain the colon; replace the numeric array */
            memmove(start+4,end+1,used-(size_t)(end+1-output)+1);
            memcpy(start,"null",4); used=strlen(output);
        }
    }
    n=clock_json(record->have_clock ? &record->clock : NULL,output+used,capacity-used);
    if(n<0 || (size_t)n>=capacity-used) return 0;
    used+=(size_t)n;
    if(!json_append(output,capacity,&used,",\"weather\":")) return 0;
    weather_batch_prepare(&record->data.w.weather);
    n=weather_batch_json(&record->data.w.weather,output+used,capacity-used);
    if(n<0) return 0;
    used+=(size_t)n;
    if(a->config.spatial_enabled) {
        if(!json_append(output,capacity,&used,",\"spatial\":")) return 0;
        n=sp_batch_json(&record->data.w.spatial,output+used,capacity-used);
        if(n<0) return 0;
        used+=(size_t)n;
    }
    return json_append(output,capacity,&used,"}]}\n") ? (int)used : 0;
}
static int encode(app *a, send_record *record, losses l, const tc_report *report,char *output,size_t capacity) {
    int k;
    size_t used;
    const double *origin=a->live ? record->origin : a->origin;
    k = snprintf(output, capacity,
        "{\"schema\":%u,\"stream\":\"%s\",\"session\":\"%s\",\"frame\":\"ENU\","
        "\"origin_wgs84\":[%.17g,%.17g,%.17g],\"time_basis\":\"pixhawk_boot_us\","
        "\"time_epoch\":\"session_boot\",\"utc_basis\":\"unix_us_estimate\","
        "\"field_order\":[\"t_us\",\"seq\",\"quality\",\"key\",\"unit\",\"east_m\",\"north_m\",\"up_m\","
        "\"ve_mps\",\"vn_mps\",\"vu_mps\",\"value\",\"time_uncert_us\",\"nav_age_us\",\"pos_error_m\",\"utc_us\",\"utc_uncert_us\"],"
        "\"loss\":{\"rejected\":\"%" PRIu64 "\",\"sequence_gaps\":\"%" PRIu64 "\","
        "\"queue_windows\":\"%" PRIu64 "\",\"queue_samples\":\"%" PRIu64 "\",\"raw_samples\":\"%" PRIu64 "\"},\"clock_model\":",
        a->live ? 3u : 2u, a->stream, a->live ? record->session : a->session, origin[0], origin[1], origin[2],
        l.rejected, l.sequence_gaps, l.queue_windows, l.queue_samples, l.raw_samples);
    if (k < 0 || (size_t)k >= capacity) return 0;
    used = (size_t)k;
    k=clock_json(report,output+used,capacity-used);
    if (k < 0 || (size_t)k >= capacity - used) return 0;
    used += (size_t)k;
    k=snprintf(output+used,capacity-used,",\"batches\":[");
    if(k<0 || (size_t)k>=capacity-used) return 0;
    used+=(size_t)k;
    if(a->live) {
        /* Insert source identity before the batches member without mutable state. */
        const char *suffix="\"batches\":[";
        size_t tail=strlen(suffix);
        if(used<tail) return 0;
        used-=tail;
        k=snprintf(output+used,capacity-used,
          "\"source\":{\"sysid\":%u,\"compid\":%u,\"epoch\":%u,\"altitude_datum\":\"MSL\"},\"batches\":[",
          (unsigned)record->system,(unsigned)record->component,record->generation);
        if(k<0 || (size_t)k>=capacity-used) return 0;
        used+=(size_t)k;
        if(record->raw_utc) {
            char uncertainty[32];
            if(record->raw_uncert==UINT32_MAX) (void)strcpy(uncertainty,"null");
            else (void)snprintf(uncertainty,sizeof(uncertainty),"%u",record->raw_uncert);
            used-=strlen("\"batches\":[");
            k=snprintf(output+used,capacity-used,"\"raw_clock_anchor\":{\"boot_us\":\"%" PRIu64 "\",\"utc_us\":\"%" PRIu64 "\",\"uncert_us\":%s},\"batches\":[",
                record->raw_boot,record->raw_utc,uncertainty);
            if(k<0 || (size_t)k>=capacity-used) return 0;
            used+=(size_t)k;
        }
    }
    {
        tb_batch_prepare(&record->data.telemetry);
        k = tb_batch_json_mode(&record->data.telemetry, output + used,
                               capacity - used, !a->summary);
        if (k < 0) return 0;
        if(a->live) {
            char prefix[1536];
            int part,prefix_n=snprintf(prefix,sizeof(prefix),"{\"batch_id\":\"%" PRIu64 "\",\"clock_model\":",record->batch_id);
            if(prefix_n<1 || (size_t)prefix_n>=sizeof(prefix)) return 0;
            part=clock_json(record->have_clock ? &record->clock : NULL,
                            prefix+prefix_n,sizeof(prefix)-(size_t)prefix_n);
            if(part<0 || (size_t)part>=sizeof(prefix)-(size_t)prefix_n) return 0;
            prefix_n+=part;
            if(record->raw_utc) {
                char uncertainty[32];
                if(record->raw_uncert==UINT32_MAX) (void)strcpy(uncertainty,"null");
                else (void)snprintf(uncertainty,sizeof(uncertainty),"%u",record->raw_uncert);
                part=snprintf(prefix+prefix_n,sizeof(prefix)-(size_t)prefix_n,
                    ",\"raw_clock_anchor\":{\"boot_us\":\"%" PRIu64 "\",\"utc_us\":\"%" PRIu64 "\",\"uncert_us\":%s}",
                    record->raw_boot,record->raw_utc,uncertainty);
                if(part<0 || (size_t)part>=sizeof(prefix)-(size_t)prefix_n) return 0;
                prefix_n+=part;
            }
            if((size_t)prefix_n+1>=sizeof(prefix) || (size_t)k+(size_t)prefix_n+1>=capacity-used) return 0;
            prefix[prefix_n++]=',';
            memmove(output+used+prefix_n,output+used+1,(size_t)k);
            memcpy(output+used,prefix,(size_t)prefix_n);
            k+=prefix_n-1;
        }
        used += (size_t)k;
    }
    if (used + 3 >= capacity) return 0;
    memcpy(output + used, "]}\n", 4);
    return (int)(used + 3);
}
/* Every socket use is under the app mutex. The socket is nonblocking;
 * DNS/reopen runs elsewhere and HTTP never shares this path. */
static void send_backup_locked(app *a,const char *json,size_t length) {
    ub_report before,after; int ok;
    if(!a->config.udp_backup_enabled) return;
    if(!a->backup_socket) { ++a->diag.backup_dropped; return; }
    ub_sender_get_report(a->backup_socket,&before);
    ok=ub_sender_send(a->backup_socket,json,length);
    ub_sender_get_report(a->backup_socket,&after);
    a->diag.backup_messages+=after.messages-before.messages;
    a->diag.backup_datagrams+=after.datagrams-before.datagrams;
    a->diag.backup_bytes+=after.bytes-before.bytes;
    a->diag.backup_errors+=after.errors-before.errors;
    a->diag.backup_dropped+=after.drops-before.drops;
    a->diag.backup_error=ok ? 0 : after.last_error;
    if(ok) a->diag.backup_last_sent_ms=now_ms();
}
static int publish_record(app *a,unsigned samples) {
    send_record *record=&a->scratch;
    mq_metadata metadata={record->queued_at,(unsigned)record->kind,samples};
    mq_result result;
    int bytes=record->kind==RECORD_WEATHER ? encode_weather(a,record,backup_encoded,sizeof(backup_encoded)) :
        record->kind==RECORD_TEST ? encode_test(a,record,backup_encoded,sizeof(backup_encoded)) :
        encode(a,record,a->loss,record->have_clock ? &record->clock : NULL,backup_encoded,sizeof(backup_encoded));
    if(bytes<=0) { a->failed=1; return 0; }
    /* UDP is attempted before storage, even when TCP has exhausted its budget. */
    send_backup_locked(a,backup_encoded,(size_t)bytes);
    result=mq_push(&a->queue,metadata,backup_encoded,(size_t)bytes);
    if(result==MQ_OK) return 1;
    if(!a->restart_requested) { ++a->loss.queue_windows; a->loss.queue_samples+=samples; }
    if(result==MQ_ALLOC_FAILED) ++a->diag.tcp_allocation_failures;
    return 0;
}
/* Merge only identical envelope prefixes. Each record's bytes and ID remain
 * immutable; different metadata/session prefixes get their own request. */
static size_t record_prefix(const mq_record *record) {
    const char *marker=record->meta.kind==RECORD_TELEMETRY ? ",\"batches\":[" : ",\"records\":[";
    const char *start=strstr(record->json,marker);
    return start ? (size_t)(start-record->json)+strlen(marker) : 0;
}
/* A mixed request nests the same immutable JSON envelopes. Keep the existing
 * weather/telemetry grouping when both kinds are due in one collection cycle. */
static int encode_pending_bundle(app *a,unsigned *count) {
    const mq_record *record=a->queue.head;
    unsigned n=0; size_t used=0;
    if(!json_append(wire,sizeof(wire),&used,"{\"schema\":\"mavlink_router_bundle_v1\",\"records\":[")) return 0;
    while(record && n<*count) {
        const char *kind;
        if(record->meta.kind==RECORD_WEATHER) kind="weather";
        else if(record->meta.kind==RECORD_TELEMETRY) kind="telemetry";
        else break;
        if(record->length+64>=sizeof(wire)-used) break;
        if(!json_append(wire,sizeof(wire),&used,"%s{\"kind\":\"%s\",\"payload\":",n ? "," : "",kind)) return 0;
        memcpy(wire+used,record->json,record->length); used+=record->length;
        wire[used++]='}'; ++n; record=record->next;
    }
    if(!n || !json_append(wire,sizeof(wire),&used,"]}\n")) return 0;
    *count=n; return (int)used;
}
static int encode_pending(app *a,unsigned *count) {
    const mq_record *first=a->queue.head,*record=first;
    size_t prefix,used=0;
    unsigned n=0,limit=*count;
    if(!first || !(prefix=record_prefix(first))) return 0;
    if(a->live && !a->test_mode) {
        const mq_record *scan=first;
        unsigned i;
        for(i=0;scan && i<limit;++i,scan=scan->next)
            if(scan->meta.kind!=first->meta.kind) return encode_pending_bundle(a,count);
    }
    if(prefix>=sizeof(wire)) return 0;
    memcpy(wire,first->json,prefix); used=prefix;
    while(record && n<limit) {
        size_t current_prefix=record_prefix(record),content;
        if(record->meta.kind!=first->meta.kind || current_prefix!=prefix ||
           memcmp(record->json,first->json,prefix)) break;
        if(record->length<prefix+3 || memcmp(record->json+record->length-3,"]}\n",3)) return 0;
        content=record->length-prefix-3;
        if(content+5>sizeof(wire)-used) break;
        if(n) wire[used++]=',';
        memcpy(wire+used,record->json+prefix,content); used+=content;
        ++n; record=record->next;
    }
    if(!n) return 0;
    memcpy(wire+used,"]}\n",4); *count=n;
    return (int)(used+3);
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
/* Caller holds app mutex; HTTP captures route identity in the same transaction. */
static void snapshot_locked(app *a, tm_snapshot *out, uint64_t now) {
    *out = a->diag;
    out->now_ms = now;
    out->eof = a->eof; out->failed = a->failed;
    out->rejected = a->loss.rejected;
    out->sequence_gaps = a->loss.sequence_gaps;
    out->raw_dropped = a->loss.raw_samples;
    out->queue_windows_dropped = a->loss.queue_windows;
    out->queue_samples_dropped = a->loss.queue_samples;
    out->queue_count = a->queue.count;
    out->tcp_buffer_bytes=a->queue.bytes; out->tcp_buffer_limit=a->queue.limit;
    out->tcp_buffer_high_water=a->queue.peak;
    out->current_raw = a->current.n ? a->current.raw_n : 0;
    out->current_observations = a->current.n;
    out->watermark_us = a->watermark_us;
    out->current_window_us = a->current.n ? a->current.window_us : 0;
    out->raw_capacity = TB_CAPACITY; out->send_capacity = SEND_WINDOWS;
    out->oldest_age_ms = a->queue.count ? now - a->queue.head->meta.queued_ms : 0;
    out->last_boot_us = a->live ? a->diag.last_boot_us : (a->have_last ? a->last_t : 0);
    out->have_clock = tc_get_report(&a->clock, &out->clock);
    out->spatial_known=a->spatial.report.path_known;
    out->spatial_effective_m=a->spatial.report.effective_width_m;
    out->spatial_distance_m=a->spatial.report.s_m;
    out->spatial_open=a->spatial.report.open_n;
    out->spatial_completed=a->spatial.report.completed_records;
    out->spatial_unknown=a->spatial.report.unknown_samples;
    out->spatial_dropped=a->spatial.report.dropped_records;
    out->spatial_undersampled=a->spatial.report.undersampled_steps;
    out->spatial_flags=a->spatial.report.flags;
}
static void snapshot(void *context, tm_snapshot *out) {
    app *a=context;
    lock(&a->mutex);
    snapshot_locked(a,out,now_ms());
    unlock(&a->mutex);
}
/* HTTP and UDP status share encoding and one monotonic identity sequence.
 * Neither path can acknowledge or remove observation records. */
static int encode_status_snapshot(app *a,char *output,size_t capacity,int http_attempt) {
    tm_snapshot current;
    char session[sizeof(a->active_session)];
    uint64_t generation,sequence;
    lock(&a->mutex);
    if(a->eof || a->restart_requested || a->failed || shutdown_requested()) {
        unlock(&a->mutex); return 0;
    }
    snapshot_locked(a,&current,now_ms());
    memcpy(session,a->active_session,sizeof(session)); generation=a->route_generation;
    sequence=++a->status_next_seq;
    if(http_attempt) { ++a->diag.status_attempts; a->diag.last_status_attempt_ms=current.now_ms; }
    unlock(&a->mutex);
    return sr_encode(&current,&a->config,a->stream,session,generation,sequence,output,capacity);
}
static void send_status(app *a,http *h) {
    long status=0; unsigned error=0;
    int bytes=encode_status_snapshot(a,wire,sizeof(wire)<SR_WIRE_CAP ? sizeof(wire) : SR_WIRE_CAP,1);
    int ok;
    if(!bytes) return;
    ok=bytes>0 && http_post_snapshot(a,h,wire,(size_t)bytes,&status,&error);
    if(bytes<0) error=EOVERFLOW;
    lock(&a->mutex);
    a->diag.status_http_status=status; a->diag.status_error_code=error;
    if(ok) { ++a->diag.status_acks; a->diag.last_status_success_ms=now_ms(); }
    else ++a->diag.status_failures;
    unlock(&a->mutex);
}
#ifdef _WIN32
static DWORD WINAPI backup_worker(void *context)
#else
static void *backup_worker(void *context)
#endif
{
    app *a=context;
    char host[256],error_text[160]; unsigned port=0,error=0;
    uint64_t reopen_at=0,next_status=now_ms()+a->config.status_interval_ms;
    if(!cfg_udp_backup_destination(&a->config,host,sizeof(host),&port,error_text,sizeof(error_text))) return 0;
    for(;;) {
        uint64_t now=now_ms(); int stopped,collecting,need_socket;
        lock(&a->mutex);
        stopped=a->backup_stop;
        collecting=!a->eof && !a->restart_requested && !a->failed && !shutdown_requested();
        if(!stopped && collecting) seal_due(a,now,0,0);
        need_socket=a->backup_socket==NULL;
        unlock(&a->mutex);
        if(stopped) break;
        if(need_socket && now>=reopen_at) {
            ub_sender *socket=ub_sender_open(host,port,a->run_nonce,&error);
            reopen_at=now_ms()+BACKUP_REOPEN_MS;
            lock(&a->mutex);
            a->backup_socket=socket; a->diag.backup_ready=socket!=NULL;
            if(!socket) { ++a->diag.backup_errors; a->diag.backup_error=error; }
            unlock(&a->mutex);
        }
        if(collecting && a->live && a->config.status_interval_ms && now>=next_status) {
            int bytes=encode_status_snapshot(a,backup_output,sizeof(backup_output)<SR_WIRE_CAP ? sizeof(backup_output) : SR_WIRE_CAP,0);
            if(bytes>0) {
                lock(&a->mutex); send_backup_locked(a,backup_output,(size_t)bytes); unlock(&a->mutex);
            }
            next_status=now+a->config.status_interval_ms;
        }
        pause_ms(BACKUP_TICK_MS);
    }
    return 0;
}
/* The monitor owns the draft. Keep configuration immutable for all workers:
 * the main thread applies this copy only after input, sender and TUI stop. */
static int request_restart(void *context,const tb_config *next,char *error,size_t cap) {
    app *a=context;
    int accepted=0;
    lock(&a->mutex);
    if(!a->live || !next || strcmp(next->input_mode,"mavlink"))
        (void)snprintf(error,cap,"In-app restart requires MAVLink input; CSV replay cannot restart.");
    else if(a->restart_requested || a->eof || a->failed || shutdown_requested())
        (void)snprintf(error,cap,"Receiver is already restarting or stopping.");
    else if(cfg_save(a->config_path,next,SEND_WINDOWS,error,cap)) {
        a->restart_config=*next;
        /* Never disclose a new destination's credentials to the old endpoint. */
        if(!strcmp(a->config.url,next->url)) a->auth=resolve_http_auth(next);
        a->restart_requested=1; a->restart_blocked=0;
        ++a->restart_attempt;
        a->diag.restarting=1;
        (void)snprintf(a->diag.restart_notice,sizeof(a->diag.restart_notice),
                       "Restart requested; stopping input and draining pending data.");
        accepted=1;
    }
    unlock(&a->mutex);
    return accepted;
}
#ifdef _WIN32
static DWORD WINAPI monitor_worker(void *context)
#else
static void *monitor_worker(void *context)
#endif
{
    app *a = context;
    tm_run(a, snapshot, &a->config, a->config_path, request_restart);
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
    unsigned failures = 0, drain_failures = 0, restart_attempt = 0, due=0;
    uint64_t interval = a->config.send_interval_ms;
    uint64_t next = now_ms() + interval;
    uint64_t status_interval=a->config.status_interval_ms;
    uint64_t next_status=now_ms()+status_interval;
    int dry = strcmp(a->url, "-") == 0;
    int status_enabled=!dry && a->live && status_interval!=0;
    int was_draining=0,http_ready=0,prefer_measurements=0;
    memset(&h, 0, sizeof(h));
    http_ready=dry || http_open(&h,&a->config);
    if(!http_ready && !a->config.udp_backup_enabled) {
        fprintf(stderr, "HTTP setup failed\n");
        lock(&a->mutex); a->failed = 1; unlock(&a->mutex);
        http_close(&h);
        return 0;
    }
    lock(&a->mutex); a->ready = 1; unlock(&a->mutex);
    for (;;) {
        uint64_t now = now_ms(), completed;
        unsigned n;
        int eof, restarting, bytes, ok, draining, pending, finished, show_errors = 1;
        long status = 0;
        unsigned error = 0;
        double encode_start, encode_ms, post_start, post_ms;
        lock(&a->mutex);
        draining=a->eof || a->restart_requested;
        pending=a->queue.count || unsealed_data(a);
        finished=a->eof && !pending;
        /* No waiting is needed for the first/final empty cycle. If a POST was
         * just attempted, preserve its minimum retry interval across EOF. */
        if(draining && !was_draining &&
           (!a->diag.net_attempts || now-a->diag.last_attempt_ms>=interval ||
            (!a->queue.count && !unsealed_data(a)))) next=now;
        unlock(&a->mutex);
        was_draining=draining;
        if(finished) break;
        if(!dry) {
            /* GetTickCount64 can quantize by ~16 ms. Use the existing precise
             * monotonic clock so a nominal 500 ms gap cannot become 484 ms. */
            double wait=a->pacing.next_ms-precise_ms();
            if(wait>0) {
                pause_ms((unsigned)(wait>100 ? 100 : ceil(wait)));
                continue;
            }
        }
        if(!http_ready && now>=next) {
            http_close(&h); memset(&h,0,sizeof(h));
            http_ready=http_open(&h,&a->config);
            lock(&a->mutex);
            if(!http_ready) { ++a->diag.net_failures; a->diag.error_code=EIO; }
            unlock(&a->mutex);
            if(!http_ready) {
                if(draining) {
                    int eof_now;
                    lock(&a->mutex);
                    eof_now=a->eof;
                    if(a->config.spatial_enabled) sp_break(&a->spatial,SP_FLAG_CONTEXT_CHANGE);
                    seal_due(a,now_ms(),1,a->restart_requested);
                    if(a->restart_requested) a->restart_blocked=1;
                    if(eof_now) a->failed=1;
                    unlock(&a->mutex);
                    if(eof_now) break;
                }
                next=now_ms()+interval; pause_ms(BACKUP_TICK_MS); continue;
            }
        }
        if(status_enabled && http_ready && !draining && now>=next_status &&
           (!prefer_measurements || !pending || now<next)) {
            uint64_t attempt_start=now;
            send_status(a,&h);
            now=now_ms();
            next_status=attempt_start+status_interval;
            /* A slow HTTP request skips missed status slots, never catches up
             * by posting old snapshots or a burst of immediate requests. */
            if(next_status<=now) next_status=now+status_interval;
            /* When both are overdue, alternate with measurements so a long
             * configured gap cannot let status reports starve the FIFO. */
            prefer_measurements=1;
            if(precise_ms()<a->pacing.next_ms) continue;
        }
        if(now<next) {
            uint64_t wake=next;
            if(status_enabled && !draining && next_status>now && next_status<wake) wake=next_status;
            pause_ms((unsigned)(wake-now>100 ? 100 : wake-now));
            continue;
        }
        lock(&a->mutex);
        restarting=a->restart_requested;
        if(restarting && restart_attempt!=a->restart_attempt) {
            restart_attempt=a->restart_attempt; drain_failures=0;
        }
        if((a->eof || restarting) && a->config.spatial_enabled) sp_break(&a->spatial,SP_FLAG_CONTEXT_CHANGE);
        /* A restart pauses collection. Let already queued windows drain before
         * sealing final partial records; a full buffer must not drop them. */
        seal_due(a,now,a->eof || restarting,restarting);
        if(!due) due=a->queue.count;
        n=due<a->config.max_windows ? due : a->config.max_windows;
        if(n>a->queue.count) n=a->queue.count;
        if(a->live && !a->test_mode && n==1 && due>=2 && a->queue.count>=2 && SEND_WINDOWS>=2 &&
           a->queue.head->meta.kind!=a->queue.head->next->meta.kind) n=2;
        eof=a->eof;
        encode_start=precise_ms();
        bytes=n ? encode_pending(a,&n) : 0;
        unlock(&a->mutex);
        if(!n && eof) break;
        encode_ms = precise_ms() - encode_start;
        if (n && !bytes) {
            fprintf(stderr, "JSON capacity exceeded\n");
            lock(&a->mutex); a->failed = 1; unlock(&a->mutex);
            break;
        }
        if (n) {
            prefer_measurements=0;
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
                        : http_post_snapshot(a, &h, wire, (size_t)bytes, &status, &error));
        post_ms = precise_ms() - post_start;
        completed = now_ms();
        if (n) {
            lock(&a->mutex);
            a->diag.inflight_windows = 0;
            a->diag.http_status = status; a->diag.error_code = error;
            a->diag.last_rtt_ms = post_ms;
            if (ok) {
                mq_release(&a->queue,n);
                due=due>=n ? due-n : 0;
                if (dry) ++a->diag.dry_releases;
                else ++a->diag.net_acks;
                a->diag.last_success_ms = completed;
                failures = 0; drain_failures = 0;
            } else {
                ++failures;
                ++a->diag.net_failures;
                if(eof || restarting) ++drain_failures;
            }
            a->diag.consecutive_failures = failures;
            show_errors = !a->monitor_enabled;
            unlock(&a->mutex);
            if (!ok) {
                if (show_errors)
                    fprintf(stderr, "Send failed; retaining %u window(s), attempt %u\n", n, failures);
                if ((eof || restarting) && (restarting ? drain_failures : failures) >= FINAL_SEND_ATTEMPTS) {
                    lock(&a->mutex);
                    if(restarting && restart_attempt==a->restart_attempt) a->restart_blocked=1;
                    else if(eof) a->failed=1;
                    unlock(&a->mutex);
                    if(eof) break;
                }
            }
        }
        /* ACKed backlog remains due, but every POST shares the gate above.
         * Failed attempts keep the prefix and the configured retry cadence.
         * Newly arriving records join the next collection cycle. */
        next = ok && due ? completed : (completed > now + interval ? completed : now + interval);
    }
    if (!dry) http_close(&h);
    return 0;
}
static void receive_csv(app *a) {
    char line[1024];
    while(fgets(line,sizeof(line),stdin)) {
        tb_sample sample;
        char *p=line;
        int bad=0;
        size_t input_len=strlen(line);
        if(!strchr(line,'\n') && !feof(stdin)) {
            int c;
            while((c=getchar())!='\n' && c!=EOF) ++input_len;
            if(c=='\n') ++input_len;
            bad=1;
        }
        lock(&a->mutex);
        a->diag.input_bytes+=input_len; ++a->diag.input_lines;
        a->diag.last_receipt_ms=now_ms();
        unlock(&a->mutex);
        while(isspace((unsigned char)*p)) ++p;
        if(!*p || *p=='#') continue;
        if(!bad && *p=='@') {
            if(!control(a,p)) { lock(&a->mutex); ++a->loss.rejected; unlock(&a->mutex); }
        } else if(!bad && parse(p,&sample)) ingest(a,&sample);
        else { lock(&a->mutex); ++a->loss.rejected; unlock(&a->mutex); }
        lock(&a->mutex); bad=a->failed; unlock(&a->mutex);
        if(bad) break;
    }
}
static int runtime_initialize(app *a,uint64_t started_ms,unsigned restarts) {
    sp_options options;
    a->auth=resolve_http_auth(&a->config);
    mq_init(&a->queue,(size_t)a->config.tcp_buffer_max_mb*CFG_MIB_BYTES);
    tc_init(&a->clock);
    sp_options_default(&options);
    options.step_m=a->config.spatial_step_m;
    options.gap_us=(uint64_t)a->config.spatial_gap_ms*1000;
    options.adaptive=a->config.spatial_adaptive;
    sp_init(&a->spatial,&options);
    a->live=!strcmp(a->config.input_mode,"mavlink");
    if((a->live || a->config.udp_backup_enabled) && !run_nonce(&a->run_nonce)) {
        fprintf(stderr,"Could not generate unique router run ID\n");
        a->failed=1; return 0;
    }
    a->last_seal_ms=a->weather_last_seal_ms=now_ms();
    a->url=a->config.url; a->stream=a->config.stream;
    a->session=a->config.session; a->summary=a->config.summary;
    if(a->live) format_session(a);
    memcpy(a->origin,a->config.origin,sizeof(a->origin));
    a->origin_known=!a->config.origin_auto;
    a->diag.started_ms=started_ms;
    a->diag.dry_run=!strcmp(a->url,"-");
    a->diag.test_mode=a->test_mode;
    a->diag.config_restarts=restarts;
    if(a->config.udp_backup_enabled) {
        char host[256],message[160]; unsigned port,error=0;
        if(cfg_udp_backup_destination(&a->config,host,sizeof(host),&port,message,sizeof(message))) {
            (void)snprintf(a->diag.backup_host,sizeof(a->diag.backup_host),"%s",host);
            a->diag.backup_port=port;
            a->backup_socket=ub_sender_open(host,port,a->run_nonce,&error);
            a->diag.backup_ready=a->backup_socket!=NULL;
            if(!a->backup_socket) { ++a->diag.backup_errors; a->diag.backup_error=error; }
        }
    }
    if(restarts)
        (void)snprintf(a->diag.restart_notice,sizeof(a->diag.restart_notice),
                       "Settings applied; receiver restarted in this process.");
    return 1;
}
typedef enum { RUNTIME_FINISHED, RUNTIME_RESTART } runtime_result;
/* A configuration has exactly one input owner and one sender. Reconfiguration
 * never creates a second process or hands stdin back to an invoking shell. */
static runtime_result runtime_run(app *a) {
    worker_t send_thread,monitor_thread,backup_thread;
    int monitor_started=0,backup_started=0;
    runtime_result result=RUNTIME_FINISHED;
    if(!mutex_init(&a->mutex)) { a->failed=1; return result; }
    if(!worker_start(&send_thread,sender,a)) {
        a->failed=1; mutex_destroy(&a->mutex); return result;
    }
    if(a->config.udp_backup_enabled) {
        backup_started=worker_start(&backup_thread,backup_worker,a);
        if(!backup_started) {
            lock(&a->mutex); ++a->diag.backup_errors; a->diag.backup_error=EAGAIN; unlock(&a->mutex);
        }
    }
    if(a->monitor_enabled) monitor_started=worker_start(&monitor_thread,monitor_worker,a);
    if(a->monitor_enabled && !monitor_started) {
        fprintf(stderr,"Monitor could not start; receiver continues\n");
        lock(&a->mutex); a->monitor_enabled=0; unlock(&a->mutex);
    }
    for(;;) {
        int setup_failed;
        /* Validate sender setup before blocking on input. The monitor remains
         * active while an unsuccessful restart resumes this same old runtime. */
        for(;;) {
            int ready;
            lock(&a->mutex); ready=a->ready; setup_failed=a->failed; unlock(&a->mutex);
            if(ready || setup_failed) break;
            pause_ms(10);
        }
        if(!setup_failed) {
            if(a->live) {
                if(!input_stop(a)) (void)ri_run(&a->config,mavlink_event,input_status,input_stop,a);
            }
            else receive_csv(a);
        }
        lock(&a->mutex);
        if(!a->live && ferror(stdin)) a->failed=1;
        a->eof=1;
        if(a->restart_requested)
            (void)snprintf(a->diag.restart_notice,sizeof(a->diag.restart_notice),
                           "Restart waiting for pending data to be acknowledged.");
        unlock(&a->mutex);
        worker_join(send_thread);
        lock(&a->mutex);
        if(a->restart_requested && !a->failed && !shutdown_requested()) {
            int pending=a->queue.count || unsealed_data(a) || a->diag.inflight_windows;
            if(!a->restart_blocked && !pending) result=RUNTIME_RESTART;
            else {
                /* Keep every queue slot, ID, session, origin and old setting.
                 * A saved draft is not permission to discard an unacked batch. */
                a->restart_blocked=1;
                a->eof=0; a->ready=0;
                /* input_stop resumes only after old partials are safely queued,
                 * before ri_run can reset decoder or source context. */
                unlock(&a->mutex);
                if(worker_start(&send_thread,sender,a)) continue;
                lock(&a->mutex); a->failed=1;
            }
        }
        a->diag.stop=1;
        a->diag.restarting=0;
        unlock(&a->mutex);
        break;
    }
    lock(&a->mutex); a->backup_stop=1; unlock(&a->mutex);
    if(backup_started) worker_join(backup_thread);
    ub_sender_close(a->backup_socket); a->backup_socket=NULL;
    if(monitor_started) worker_join(monitor_thread);
    mutex_destroy(&a->mutex);
    return result;
}
static void apply_restart(app *a) {
    tb_config next=a->restart_config;
    losses cumulative_loss=a->loss;
    uint64_t status_sequence=a->status_next_seq;
    http_pacing pacing=a->pacing;
    const char *path=a->config_path;
    int monitor_enabled=a->monitor_enabled,test_mode=a->test_mode;
    /* Called only after the old runtime is drained and every worker joined. */
    memset(a,0,sizeof(*a));
    a->config=next; a->config_path=path;
    a->loss=cumulative_loss;
    a->status_next_seq=status_sequence;
    a->pacing=pacing;
    if(pacing.completed_ms && a->pacing.next_ms<pacing.completed_ms+next.http_min_interval_ms)
        a->pacing.next_ms=pacing.completed_ms+next.http_min_interval_ms;
    a->monitor_enabled=monitor_enabled; a->test_mode=test_mode;
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
    char error[160];
    unsigned i,restarts=0;
    int option_start,server_override=0;
    uint64_t started_ms;
#ifdef _WIN32
    char argument_storage[32768];
    char *argument_pointers[32];
    if (!utf8_args(&argc, &argv, argument_storage, sizeof(argument_storage),
                   argument_pointers, 32)) return 1;
#endif
    (void)setlocale(LC_NUMERIC, "C");
    if(argc==2 && !strcmp(argv[1],"--version")) {
        puts(TB_PRODUCT " " TB_VERSION); return 0;
    }
    cfg_defaults(&a->config);
    a->config.max_windows = SEND_WINDOWS;
    a->monitor_enabled = TB_MONITOR_DEFAULT;
    a->config_path = default_config();
    if (argc == 2 && strcmp(argv[1], "--sizes") == 0) {
        printf("sample=%zu batch=%zu state=%zu wire=%zu bundle=%u input_storage=%zu monitor_storage=%zu auxiliary_storage=%zu static_total=%zu\n",
               sizeof(tb_sample), sizeof(tb_batch), sizeof(state), sizeof(wire), 0u, ri_storage_bytes(), tm_screen_storage_bytes(), sizeof(default_config_path)+sizeof(stop_requested)+sizeof(backup_encoded)+sizeof(backup_output), sizeof(state) + sizeof(wire) + 0u + ri_storage_bytes()+tm_screen_storage_bytes()+sizeof(default_config_path)+sizeof(stop_requested)+sizeof(backup_encoded)+sizeof(backup_output));
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
                        "TUI: e edit, Enter validate, s save, r save and restart; telemetry stays in memory.\n");
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
    if(!strcmp(a->config.input_mode,"mavlink")) input_signals();
#ifndef _WIN32
    /* Initialize before the input and sender use libcurl concurrently. */
    if(curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK) return 1;
#endif
    started_ms=now_ms();
    while(runtime_initialize(a,started_ms,restarts)) {
        if(runtime_run(a)!=RUNTIME_RESTART || shutdown_requested()) break;
        apply_restart(a);
        ++restarts;
    }
#ifndef _WIN32
    curl_global_cleanup();
#endif
    fprintf(stderr, "rejected=%" PRIu64 " sequence_gaps=%" PRIu64 " raw_dropped=%" PRIu64
            " queue_windows_dropped=%" PRIu64 " queue_samples_dropped=%" PRIu64 " pending=%u\n",
            a->loss.rejected, a->loss.sequence_gaps, a->loss.raw_samples,
            a->loss.queue_windows, a->loss.queue_samples, a->queue.count);
    fprintf(stderr,"tcp_buffer bytes=%zu limit=%zu high_water=%zu allocation_failures=%" PRIu64 "\n",
            a->queue.bytes,a->queue.limit,a->queue.peak,a->diag.tcp_allocation_failures);
    mq_clear(&a->queue);
    return a->failed || a->loss.rejected || a->loss.sequence_gaps || a->loss.raw_samples || a->loss.queue_windows ? 2 : 0;
}
