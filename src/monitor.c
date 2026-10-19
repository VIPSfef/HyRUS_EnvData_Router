/* Fixed-size diagnostic TUI. This module never reads the CSV stdin stream. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "monitor.h"
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#endif

enum { KEY_NONE=0, KEY_UP=256, KEY_DOWN, KEY_ENTER, KEY_ESCAPE, KEY_BACK, KEY_CLEAR };
enum { FIELD_COUNT=33 };
static const char *const fields[FIELD_COUNT]={
    "url","stream","session","token_env","origin_lat","origin_lon","origin_alt",
    "send_interval_ms","connect_timeout_ms","io_timeout_ms","max_windows",
    "refresh_ms","idle_warn_ms","ack_warn_ms","summary",
    "input_mode","serial_port","serial_baud","websocket_url","auto_scan","probe_ms",
    "link_timeout_ms","reconnect_ms","target_system","target_component",
    "system_time_uncert_us","accel_bound_mps2","duration_ms","unit_map","origin_auto",
    "udp_bind","udp_port","udp_enabled"
};
typedef struct {
    int ansi, input_ready, csv_console;
    unsigned cleanup_mask;
    unsigned escape;
    uint64_t escape_ms;
#ifdef _WIN32
    HANDLE input, output;
    DWORD old_input, old_output;
    int input_mode, output_mode, cursor_saved;
    CONSOLE_CURSOR_INFO cursor;
#else
    int fd, saved_mode;
    struct termios saved;
#endif
} terminal;
typedef struct {
    int open, editing, selected, dirty, saved, scroll, show_devices, device_scroll;
    char number[3], input[2048], notice[160];
    size_t input_n;
    tb_config draft, saved_config;
} editor;
typedef struct { double accepted, bytes, key[TB_MAX_KEYS]; } rates;

static uint64_t mono_ms(void)
{
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec t;
    if(clock_gettime(CLOCK_MONOTONIC,&t)) return 0;
    return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;
#endif
}
static void wait_tick(void)
{
#ifdef _WIN32
    Sleep(25);
#else
    struct timespec t={0,25000000};
    while(nanosleep(&t,&t) && errno==EINTR) {}
#endif
}
static uint64_t elapsed(uint64_t now,uint64_t before)
{ return now>=before ? now-before : 0; }
static uint64_t delta(uint64_t now,uint64_t before)
{ return now>=before ? now-before : 0; }


/* Handler-visible state is copied before registration and never changed while
 * handlers are installed. No pointer to the monitor thread's stack is kept. */
static terminal restore_terminal;
#ifdef _WIN32
static void restore_console(const terminal *t)
{
    DWORD written;
    if(t->input_mode) (void)SetConsoleMode(t->input,t->old_input);
    if(t->ansi) (void)WriteFile(t->output,"\033[0m",4,&written,NULL);
    if(t->cursor_saved) (void)SetConsoleCursorInfo(t->output,&t->cursor);
    else if(t->ansi) (void)WriteFile(t->output,"\033[?25h",6,&written,NULL);
    if(t->output_mode) (void)SetConsoleMode(t->output,t->old_output);
}
static BOOL WINAPI restore_control(DWORD event)
{
    if(event==CTRL_C_EVENT || event==CTRL_BREAK_EVENT || event==CTRL_CLOSE_EVENT ||
       event==CTRL_LOGOFF_EVENT || event==CTRL_SHUTDOWN_EVENT)
        restore_console(&restore_terminal);
    /* Preserve the previously registered handlers and default termination. */
    return FALSE;
}
static void restore_install(terminal *t)
{
    if(!t->input_mode && !t->output_mode) return;
    restore_terminal=*t;
    if(SetConsoleCtrlHandler(restore_control,TRUE)) t->cleanup_mask=1;
    else {
        restore_console(t);
        t->ansi=0; t->input_ready=0;
    }
}
static void restore_uninstall(const terminal *t)
{
    if(t->cleanup_mask) (void)SetConsoleCtrlHandler(restore_control,FALSE);
}
#else
static struct sigaction restore_int,restore_term;
/* POSIX async-signal-safe interfaces only:
 * https://pubs.opengroup.org/onlinepubs/9699919799/functions/V2_chap02.html
 * Preserve errno and the prior sigaction, then redeliver the same signal. */
static void restore_signal(int signal_number)
{
    int saved_errno=errno;
    if(restore_terminal.saved_mode)
        (void)tcsetattr(restore_terminal.fd,TCSANOW,&restore_terminal.saved);
    if(restore_terminal.ansi)
        (void)write(STDERR_FILENO,"\033[0m\033[?25h",10);
    if(signal_number==SIGINT) (void)sigaction(SIGINT,&restore_int,NULL);
    else (void)sigaction(SIGTERM,&restore_term,NULL);
    (void)raise(signal_number);
    errno=saved_errno;
}
static void restore_one(int signal_number,const struct sigaction *prior)
{
    struct sigaction current;
    /* Do not overwrite a handler installed by another component after ours. */
    if(!sigaction(signal_number,NULL,&current) &&
       !(current.sa_flags & SA_SIGINFO) && current.sa_handler==restore_signal)
        (void)sigaction(signal_number,prior,NULL);
}
static void restore_uninstall(const terminal *t)
{
    if(t->cleanup_mask & 1u) restore_one(SIGINT,&restore_int);
    if(t->cleanup_mask & 2u) restore_one(SIGTERM,&restore_term);
}
static void restore_install(terminal *t)
{
    struct sigaction action;
    if(!t->ansi && !t->saved_mode) return;
    if(sigaction(SIGINT,NULL,&restore_int) || sigaction(SIGTERM,NULL,&restore_term))
        goto unavailable;
    restore_terminal=*t;
    memset(&action,0,sizeof(action));
    action.sa_handler=restore_signal;
    (void)sigemptyset(&action.sa_mask);
    (void)sigaddset(&action.sa_mask,SIGINT);
    (void)sigaddset(&action.sa_mask,SIGTERM);
    /* Ignored signals remain ignored; only a terminating/caught signal needs
     * terminal cleanup. Restoring and raising preserves previous handlers. */
    if(restore_int.sa_handler!=SIG_IGN) {
        action.sa_flags=restore_int.sa_flags & SA_RESTART;
        if(sigaction(SIGINT,&action,NULL)) goto unavailable;
        t->cleanup_mask|=1u;
    }
    if(restore_term.sa_handler!=SIG_IGN) {
        action.sa_flags=restore_term.sa_flags & SA_RESTART;
        if(sigaction(SIGTERM,&action,NULL)) goto unavailable;
        t->cleanup_mask|=2u;
    }
    return;
unavailable:
    restore_uninstall(t);
    t->cleanup_mask=0;
    if(t->saved_mode) (void)tcsetattr(t->fd,TCSANOW,&t->saved);
    if(t->ansi) (void)write(STDERR_FILENO,"\033[0m\033[?25h",10);
    t->ansi=0; t->input_ready=0;
}
#endif

static void terminal_open(terminal *t, const tb_config *config)
{
    memset(t,0,sizeof(*t));
#ifdef _WIN32
    t->input=INVALID_HANDLE_VALUE;
    t->output=GetStdHandle(STD_ERROR_HANDLE);
    if(!_isatty(_fileno(stderr)) || !GetConsoleMode(t->output,&t->old_output)) return;
    t->cursor_saved=GetConsoleCursorInfo(t->output,&t->cursor)!=0;
    if(!SetConsoleMode(t->output,t->old_output|ENABLE_VIRTUAL_TERMINAL_PROCESSING)) return;
    t->output_mode=1; t->ansi=1;
    if(!strcmp(config->input_mode,"csv") && _isatty(_fileno(stdin))) { t->csv_console=1; return; }
    t->input=CreateFileW(L"CONIN$",GENERIC_READ|GENERIC_WRITE,
                        FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
    if(t->input==INVALID_HANDLE_VALUE || !GetConsoleMode(t->input,&t->old_input)) return;
    if(!SetConsoleMode(t->input,t->old_input & ~(ENABLE_LINE_INPUT|ENABLE_ECHO_INPUT))) return;
    t->input_mode=1; t->input_ready=1;
#else
    struct termios raw;
    t->fd=-1;
    if(!isatty(fileno(stderr))) return;
    t->ansi=1;
    if(!strcmp(config->input_mode,"csv") && isatty(fileno(stdin))) { t->csv_console=1; return; }
    t->fd=open("/dev/tty",O_RDWR|O_NONBLOCK|O_NOCTTY);
    if(t->fd<0 || tcgetattr(t->fd,&t->saved)) return;
    raw=t->saved;
    raw.c_lflag &= (tcflag_t)~(ICANON|ECHO);
    raw.c_cc[VMIN]=0; raw.c_cc[VTIME]=0;
    if(tcsetattr(t->fd,TCSANOW,&raw)) return;
    t->saved_mode=1; t->input_ready=1;
#endif
}
static void terminal_close(terminal *t)
{
    restore_uninstall(t);
#ifdef _WIN32
    if(t->cursor_saved) (void)SetConsoleCursorInfo(t->output,&t->cursor);
    if(t->input_mode) (void)SetConsoleMode(t->input,t->old_input);
    if(t->input!=INVALID_HANDLE_VALUE) (void)CloseHandle(t->input);
    if(t->output_mode) (void)SetConsoleMode(t->output,t->old_output);
#else
    if(t->saved_mode) (void)tcsetattr(t->fd,TCSANOW,&t->saved);
    if(t->fd>=0) (void)close(t->fd);
#endif
}
static unsigned terminal_rows(void)
{
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info;
    if(GetConsoleScreenBufferInfo(GetStdHandle(STD_ERROR_HANDLE),&info)) {
        int n=info.srWindow.Bottom-info.srWindow.Top+1;
        if(n>0) return (unsigned)n;
    }
#else
    struct winsize size;
    if(!ioctl(fileno(stderr),TIOCGWINSZ,&size) && size.ws_row) return size.ws_row;
#endif
    return 24;
}

static unsigned terminal_columns(void)
{
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info;
    if(GetConsoleScreenBufferInfo(GetStdHandle(STD_ERROR_HANDLE),&info)) {
        int n=info.srWindow.Right-info.srWindow.Left+1;
        if(n>0) return (unsigned)n;
    }
#else
    struct winsize size;
    if(!ioctl(fileno(stderr),TIOCGWINSZ,&size) && size.ws_col) return size.ws_col;
#endif
    return 80;
}
typedef struct { FILE *out; unsigned columns, used, lines; } writer;
/* One bounded line at a time, never a full screen allocation. Console lines
 * are clipped before the last column so long keys/URLs cannot wrap the frame. */
static void write_text(writer *w,const char *fmt,...)
{
    char text[2304];
    const unsigned char *p;
    va_list ap;
    va_start(ap,fmt); (void)vsnprintf(text,sizeof(text),fmt,ap); va_end(ap);
    for(p=(const unsigned char *)text;*p;++p) {
        if(*p=='\n') { (void)fputc('\n',w->out); w->used=0; ++w->lines; }
        else {
            if(!w->columns || w->used+1<w->columns) (void)fputc(*p,w->out);
            ++w->used;
        }
    }
}

static int key_read(terminal *t)
{
    if(!t->input_ready) return KEY_NONE;
#ifdef _WIN32
    unsigned limit;
    for(limit=0;limit<32;++limit) {
        DWORD count,read_n;
        INPUT_RECORD r;
        WORD vk;
        WCHAR ch;
        if(!GetNumberOfConsoleInputEvents(t->input,&count) || !count) return KEY_NONE;
        if(!ReadConsoleInputW(t->input,&r,1,&read_n) || !read_n) return KEY_NONE;
        if(r.EventType!=KEY_EVENT || !r.Event.KeyEvent.bKeyDown) continue;
        vk=r.Event.KeyEvent.wVirtualKeyCode; ch=r.Event.KeyEvent.uChar.UnicodeChar;
        if(vk==VK_UP) return KEY_UP;
        if(vk==VK_DOWN) return KEY_DOWN;
        if(vk==VK_RETURN) return KEY_ENTER;
        if(vk==VK_ESCAPE) return KEY_ESCAPE;
        if(vk==VK_BACK || vk==VK_DELETE) return KEY_BACK;
        if(ch==21) return KEY_CLEAR;
        if(ch>=32 && ch<=126) return (int)ch;
    }
    return KEY_NONE;
#else
    unsigned char ch;
    ssize_t n;
    if(t->escape && elapsed(mono_ms(),t->escape_ms)>60) {
        t->escape=0; return KEY_ESCAPE;
    }
    n=read(t->fd,&ch,1);
    if(n!=1) return KEY_NONE;
    if(t->escape==1) {
        if(ch=='[' || ch=='O') { t->escape=2; return KEY_NONE; }
        t->escape=0; return KEY_ESCAPE;
    }
    if(t->escape==2) {
        t->escape=0;
        if(ch=='A') return KEY_UP;
        if(ch=='B') return KEY_DOWN;
        return KEY_NONE;
    }
    if(ch==27) { t->escape=1; t->escape_ms=mono_ms(); return KEY_NONE; }
    if(ch=='\r' || ch=='\n') return KEY_ENTER;
    if(ch==8 || ch==127) return KEY_BACK;
    if(ch==21) return KEY_CLEAR;
    return ch>=32 && ch<=126 ? (int)ch : KEY_NONE;
#endif
}
static const char *status(const tm_snapshot *s)
{ return s->failed ? "FAILED" : s->stop ? "STOP" : s->eof ? "EOF" : "RUNNING"; }
static void notice(editor *e,const char *fmt,...)
{
    va_list ap;
    va_start(ap,fmt); (void)vsnprintf(e->notice,sizeof(e->notice),fmt,ap); va_end(ap);
}
static void compute_rates(rates *r,const tm_snapshot *s,const tm_snapshot *old,int have_old)
{
    unsigned i,j;
    double dt;
    memset(r,0,sizeof(*r));
    dt=(double)elapsed(s->now_ms,have_old ? old->now_ms : s->started_ms)/1000;
    if(dt<=0) return;
    r->accepted=(double)delta(s->accepted,have_old ? old->accepted : 0)/dt;
    r->bytes=(double)delta(s->input_bytes,have_old ? old->input_bytes : 0)/dt;
    /* At most 16*16 short comparisons, only in the monitoring thread. */
    for(i=0;i<s->key_n && i<TB_MAX_KEYS;++i) {
        uint64_t previous=0;
        if(have_old) for(j=0;j<old->key_n && j<TB_MAX_KEYS;++j)
            if(!strncmp(s->keys[i].key,old->keys[j].key,TB_KEY_BYTES)) {
                previous=old->keys[j].observations; break;
            }
        r->key[i]=(double)delta(s->keys[i].observations,previous)/dt;
    }
}
/* HEARTBEAT enums identify the device; component IDs are not device kinds. */
static const char *device_type(unsigned type)
{
    switch(type) {
    case 0: return "generic"; case 1: return "plane"; case 2: return "quadrotor";
    case 3: return "coaxial"; case 4: return "helicopter"; case 5: return "tracker";
    case 6: return "GCS"; case 7: return "airship"; case 8: return "balloon";
    case 9: return "rocket"; case 10: return "rover"; case 11: return "boat";
    case 12: return "submarine"; case 13: return "hexarotor"; case 14: return "octorotor";
    case 15: return "tricopter"; case 18: return "onboard";
    case 19: case 20: case 21: case 22: case 23: case 24: case 47: return "VTOL";
    case 26: return "gimbal"; case 27: return "ADSB"; case 29: return "dodecarotor";
    case 30: return "camera"; case 31: return "charger"; case 32: return "FLARM";
    case 33: return "servo"; case 34: return "ODID"; case 35: return "decarotor";
    case 36: return "battery"; case 37: return "parachute"; case 38: return "log";
    case 39: return "OSD"; case 40: return "IMU"; case 41: return "GPS";
    case 42: return "winch"; case 43: return "multirotor"; case 44: return "light";
    case 45: return "orbiter"; case 46: return "quadruped"; case 48: return "gripper";
    case 49: return "radio"; default: return "other";
    }
}
static const char *device_autopilot(unsigned autopilot)
{
    switch(autopilot) {
    case 0: return "generic"; case 2: return "SLUGS"; case 3: return "ArduPilot";
    case 4: return "OpenPilot"; case 8: return "none"; case 9: return "PPZ";
    case 10: return "UDB"; case 11: return "FlexiPilot"; case 12: return "PX4";
    case 13: return "SMACCMPilot"; case 14: return "AutoQuad"; case 15: return "Armazila";
    case 16: return "Aerob"; case 17: return "ASLUAV"; case 18: return "SmartAP";
    case 19: return "AirRails"; case 20: return "Reflex"; case 21: return "Flix";
    default: return "other";
    }
}
static void render_snapshot(FILE *out,const tm_snapshot *s,const tb_config *c,
                            const rates *r,unsigned first,unsigned max_keys,unsigned columns,int show_devices,int ansi)
{
    writer w={out,columns,0,0};
    char endpoint[2048], source_name[256];
    int mav=strcmp(c->input_mode,"csv")!=0;
    int compact=mav && columns;
    unsigned i,last,n=s->key_n<TB_MAX_KEYS ? s->key_n : TB_MAX_KEYS;
    uint64_t idle=elapsed(s->now_ms,s->last_receipt_ms ? s->last_receipt_ms : s->started_ms);
    uint64_t ack_age=elapsed(s->now_ms,s->last_success_ms ? s->last_success_ms : s->started_ms);
    const char *token=c->token_env[0] ? getenv(c->token_env) : NULL;
    int stale=s->have_clock && (
        (s->last_anchor_receipt_ms && elapsed(s->now_ms,s->last_anchor_receipt_ms)>TC_EXTRAPOLATE_US/1000) ||
        (s->last_boot_us>s->clock.last_boot_us && s->last_boot_us-s->clock.last_boot_us>TC_EXTRAPOLATE_US));
    const char *health=s->failed ? "FAILED" : s->inflight_windows ? "SENDING" :
        s->consecutive_failures && s->queue_count ? "RETRY" :
        idle>=c->idle_warn_ms && s->current_observations ? "WAIT_WATERMARK" :
        idle>=c->idle_warn_ms ? "IDLE" : "RECEIVING";
    const char *utc_status=!s->utc_us ? "UNKNOWN" : stale ? "STALE" : "ESTIMATED";
    cfg_safe_url(c->url,endpoint,sizeof(endpoint));
    if(strstr(s->active_source,"://") && strncmp(s->active_source,"udp://",6)) cfg_safe_url(s->active_source,source_name,sizeof(source_name));
    else (void)snprintf(source_name,sizeof(source_name),"%.*s",255,s->active_source);
    write_text(&w,"[monitor] state=%s mode=%s uptime=%.1fs boundary=%s\n",
            status(s),s->test_mode ? "TEST-HTTP" : s->dry_run ? "DRY" : "HTTP",(double)elapsed(s->now_ms,s->started_ms)/1000,
            mav ? (s->test_mode ? "MAVLink-test-HB+position" : "MAVLink") : "normalized-CSV");
    write_text(&w,"ACTIVE endpoint=%s\n",endpoint);
    if(mav && c->origin_auto)
        write_text(&w,"ACTIVE stream=%.64s session=%.64s origin=auto(GLOBAL_POSITION_INT/MSL)\n",c->stream,c->session);
    else write_text(&w,"ACTIVE stream=%.64s session=%.64s origin=(%.7g,%.7g,%.7g)\n",
            c->stream,c->session,c->origin[0],c->origin[1],c->origin[2]);
    write_text(&w,"ACTIVE interval=%ums connect=%ums io=%ums max_windows=%u summary=%d\n",
            c->send_interval_ms,c->connect_timeout_ms,c->io_timeout_ms,c->max_windows,c->summary);
    if(mav) {
        write_text(&w,"ACTIVE UDP enabled=%u bind=%.63s:%u (receive-only)\n",
                c->udp_enabled,c->udp_bind,c->udp_port);
        /* Colour is terminal-only and does not consume the bounded text width.
         * LOCKED is published only after valid MAVLink and device identity;
         * merely opening a port or seeing unidentified traffic stays yellow. */
        write_text(&w,"source status=");
        if(ansi) fputs(s->failed ? "\033[31m" :
                !s->stop && !s->eof && !strcmp(s->source_status,"LOCKED") ? "\033[32m" : "\033[33m",out);
        write_text(&w,"%.31s",s->source_status[0] ? s->source_status : "WAITING");
        if(ansi) fputs("\033[0m",out);
        write_text(&w," active=%s\n",source_name[0] ? source_name : "none");
        if(s->source_error[0]) write_text(&w,"source error=%.159s\n",s->source_error);
    }
    if(compact) write_text(&w,"input bytes=%" PRIu64 " accepted=%" PRIu64 " rejected=%" PRIu64 " rate=%.1f/s\n",
            s->input_bytes,s->accepted,s->rejected,r->accepted);
    else write_text(&w,"input bytes=%" PRIu64 " lines=%" PRIu64 " accepted=%" PRIu64 " rejected=%" PRIu64
            " rate=%.1f/s %.1fB/s\n",s->input_bytes,s->input_lines,s->accepted,s->rejected,r->accepted,r->bytes);
    if(!compact || s->sequence_gaps || s->raw_dropped || s->queue_windows_dropped || s->queue_samples_dropped)
    write_text(&w,"loss gaps=%" PRIu64 " raw=%" PRIu64 " queue_windows=%" PRIu64 " queue_samples=%" PRIu64 "\n",
            s->sequence_gaps,s->raw_dropped,s->queue_windows_dropped,s->queue_samples_dropped);
    write_text(&w,"queue windows=%u/%u inflight=%u oldest=%" PRIu64 "ms current=%u raw=%u/%u\n",
            s->queue_count,s->queue_capacity,s->inflight_windows,s->oldest_age_ms,
            s->current_observations,s->current_raw,s->raw_capacity);
    if(!compact) write_text(&w,"event boot=%" PRIu64 "us watermark=%" PRIu64 "us open_window=%" PRIu64 "us\n",
            s->last_boot_us,s->watermark_us,s->current_window_us);
    if(!compact) write_text(&w,"network attempts=%" PRIu64 " acks=%" PRIu64 " failures=%" PRIu64 " dry_releases=%" PRIu64
            " (payload requests)\n",s->net_attempts,s->net_acks,s->net_failures,s->dry_releases);
    if(compact) write_text(&w,"tx tries=%" PRIu64 " ack=%" PRIu64 " fail=%" PRIu64
            " HTTP=%ld err=%u rtt=%.0fms\n",s->net_attempts,s->net_acks,s->net_failures,
            s->http_status,s->error_code,s->last_rtt_ms);
    else write_text(&w,"HTTP status=%ld error=%u consecutive=%u bytes_attempted=%" PRIu64
            " net_rtt=%.1fms encode=%.3fms\n",
            s->http_status,s->error_code,s->consecutive_failures,s->net_sent_bytes,s->last_rtt_ms,s->last_encode_ms);
    write_text(&w,"health=%s input_idle=%" PRIu64 "ms last_ack_age=%" PRIu64 "ms%s\n",
            health,idle,ack_age,!s->dry_run && s->queue_count && ack_age>=c->ack_warn_ms ? " WARN" : "");
    if(mav) {
        write_text(&w,"MAVLink frames=%" PRIu64 " crc_bad=%" PRIu64 " unknown=%" PRIu64 "\n",
                s->parsed_frames,s->bad_crc,s->unknown_message);
        write_text(&w,"MAVLink signed_rejected=%" PRIu64 " no_nav=%" PRIu64 " pending=%u drop=%" PRIu64 "\n",
                s->signed_rejected,s->no_navigation,s->pending_observations,s->pending_overflow);
        if(compact) write_text(&w,"decode clock=%" PRIu64 " other_clock=%" PRIu64 " invalid=%" PRIu64 " late=%" PRIu64 "\n",
                s->no_clock,s->wrong_clock_component,s->invalid_value,s->late_observation);
        else {
            write_text(&w,"decode no_clock=%" PRIu64 " wrong_clock_component=%" PRIu64 "\n",
                    s->no_clock,s->wrong_clock_component);
            write_text(&w,"decode invalid_value=%" PRIu64 " late_observation=%" PRIu64 "\n",
                    s->invalid_value,s->late_observation);
        }
        write_text(&w,"selected=%u.%u gps_fix=%u devices=%u\n",
                s->selected_system,s->selected_component,s->gps_fix,s->device_n);
    } else write_text(&w,"decoder=external CRC=n/a fix=n/a boundary=normalized-CSV\n");
    if(!compact) write_text(&w,"auth token_env=%.64s token=%s (credential value hidden)\n",
            c->token_env[0] ? c->token_env : "(none)",token && token[0] ? "set" : "missing");
    if(s->have_clock) {
        if(compact) {
            write_text(&w,"UTC=%s n=%u drift=%.3gppm res=%.3gus age=",utc_status,
                    (unsigned)s->clock.count,s->clock.drift_ppm,s->clock.residual_std_us);
            if(s->last_anchor_receipt_ms) write_text(&w,"%" PRIu64 "ms\n",elapsed(s->now_ms,s->last_anchor_receipt_ms));
            else write_text(&w,"unknown\n");
        } else {
            write_text(&w,"clock UTC=%s anchors=%u drift=%.3gppm resid=%.3gus\n",
                    utc_status,(unsigned)s->clock.count,s->clock.drift_ppm,s->clock.residual_std_us);
            write_text(&w,"clock offset=%" PRId64 "us host_anchor_age=",s->clock.offset_us);
            if(s->last_anchor_receipt_ms) write_text(&w,"%" PRIu64 "ms\n",elapsed(s->now_ms,s->last_anchor_receipt_ms));
            else write_text(&w,"unknown\n");
        }
    } else write_text(&w,"clock UTC=UNKNOWN anchors=0 (boot/session observations continue)\n");
    if(s->have_position && isfinite(s->p[0]) && isfinite(s->p[1]) && isfinite(s->p[2])) {
        write_text(&w,"ENU p=(%.3f,%.3f,%.3f)m v=(%.2f,%.2f,%.2f)m/s\n",
                s->p[0],s->p[1],s->p[2],(double)s->v[0],(double)s->v[1],(double)s->v[2]);
        write_text(&w,"alignment relative_uncert=%uus nav_age=%uus pos_error=%.3gm\n",
                (unsigned)s->time_uncert_us,(unsigned)s->nav_age_us,(double)s->pos_error_m);
    } else write_text(&w,"ENU position=n/a alignment=n/a\n");
    if(!compact && (s->have_position || s->utc_us)) {
        write_text(&w,"last_observation_UTC=");
        if(s->utc_us) write_text(&w,"%" PRIu64 "us absolute_uncert=%uus\n",s->utc_us,(unsigned)s->utc_uncert_us);
        else write_text(&w,"unknown absolute_uncert=n/a\n");
    }
    if(columns) {
        unsigned rows=terminal_rows();
        unsigned available=rows>w.lines+4 ? rows-w.lines-4 : 0;
        if(max_keys>available) max_keys=available;
    }
    if(!show_devices || !columns || !mav) {
        unsigned begin=show_devices && !columns ? 0 : first;
        if(begin>n) begin=n;
        last=n-begin>max_keys ? begin+max_keys : n;
        write_text(&w,"keys %u-%u/%u (observations/s; Up/Down scroll)\n",last>begin ? begin+1 : 0,last,n);
        for(i=begin;i<last;++i) {
            const tm_key *k=&s->keys[i];
            write_text(&w,"  %-20.*s %-8.*s rate=%6.1f n=%" PRIu64 " seq=%" PRIu32 " value=",
                    TB_KEY_BYTES-1,k->key,TB_UNIT_BYTES-1,k->unit,r->key[i],k->observations,k->seq);
            if(k->have_value && isfinite(k->value)) write_text(&w,"%.9g",k->value);
            else write_text(&w,"n/a");
            write_text(&w," boot=%" PRIu64 "us\n",k->t_us);
        }
    }
    if(mav && (show_devices || !columns)) {
        unsigned count=s->device_n<TM_MAX_DEVICES ? s->device_n : TM_MAX_DEVICES;
        unsigned begin=columns ? first : 0;
        unsigned visible=columns ? max_keys : TM_MAX_DEVICES;
        if(begin>count) begin=count;
        last=count-begin>visible ? begin+visible : count;
        write_text(&w,"devices %u-%u/%u (sys.comp type autopilot mode status age)\n",
                last>begin ? begin+1 : 0,last,count);
        for(i=begin;i<last;++i) {
            const tm_device *d=&s->devices[i];
            if(d->have_heartbeat)
                write_text(&w,"  %u.%u %s(%u) %s(%u) b=%u m=%" PRIu32 " s=%u age=%" PRIu64 "ms\n",
                        (unsigned)d->sysid,(unsigned)d->compid,device_type(d->type),(unsigned)d->type,
                        device_autopilot(d->autopilot),(unsigned)d->autopilot,(unsigned)d->base_mode,
                        d->custom_mode,(unsigned)d->system_status,elapsed(s->now_ms,d->last_seen_ms));
            else write_text(&w,"  %u.%u unknown (no HEARTBEAT) age=%" PRIu64 "ms\n",
                        (unsigned)d->sysid,(unsigned)d->compid,elapsed(s->now_ms,d->last_seen_ms));
        }
    }
}
void tm_render(FILE *out,const tm_snapshot *s,const tb_config *c)
{
    rates r;
    if(!out || !s || !c) return;
    compute_rates(&r,s,s,0);
    render_snapshot(out,s,c,&r,0,TB_MAX_KEYS,0,0,0);
    (void)fflush(out);
}
static void field_value(const tb_config *c,unsigned field,char *buf,size_t cap,int reveal_url)
{
    switch(field) {
    case 0: if(reveal_url) (void)snprintf(buf,cap,"%s",c->url); else cfg_safe_url(c->url,buf,cap); break;
    case 1: (void)snprintf(buf,cap,"%s",c->stream); break;
    case 2: (void)snprintf(buf,cap,"%s",c->session); break;
    case 3: (void)snprintf(buf,cap,"%s",c->token_env); break;
    case 4: case 5: case 6: (void)snprintf(buf,cap,"%.17g",c->origin[field-4]); break;
    case 7: (void)snprintf(buf,cap,"%u",c->send_interval_ms); break;
    case 8: (void)snprintf(buf,cap,"%u",c->connect_timeout_ms); break;
    case 9: (void)snprintf(buf,cap,"%u",c->io_timeout_ms); break;
    case 10: (void)snprintf(buf,cap,"%u",c->max_windows); break;
    case 11: (void)snprintf(buf,cap,"%u",c->refresh_ms); break;
    case 12: (void)snprintf(buf,cap,"%u",c->idle_warn_ms); break;
    case 13: (void)snprintf(buf,cap,"%u",c->ack_warn_ms); break;
    case 14: (void)snprintf(buf,cap,"%d",c->summary); break;
    case 15: (void)snprintf(buf,cap,"%s",c->input_mode); break;
    case 16: (void)snprintf(buf,cap,"%s",c->serial_port); break;
    case 17: (void)snprintf(buf,cap,"%u",c->serial_baud); break;
    case 18: if(reveal_url) (void)snprintf(buf,cap,"%s",c->websocket_url); else cfg_safe_url(c->websocket_url,buf,cap); break;
    case 19: (void)snprintf(buf,cap,"%u",c->auto_scan); break;
    case 20: (void)snprintf(buf,cap,"%u",c->probe_ms); break;
    case 21: (void)snprintf(buf,cap,"%u",c->link_timeout_ms); break;
    case 22: (void)snprintf(buf,cap,"%u",c->reconnect_ms); break;
    case 23: (void)snprintf(buf,cap,"%u",c->target_system); break;
    case 24: (void)snprintf(buf,cap,"%u",c->target_component); break;
    case 25: (void)snprintf(buf,cap,"%u",c->system_time_uncert_us); break;
    case 26: (void)snprintf(buf,cap,"%.17g",c->accel_bound_mps2); break;
    case 27: (void)snprintf(buf,cap,"%u",c->duration_ms); break;
    case 28: (void)snprintf(buf,cap,"%s",c->unit_map); break;
    case 29: (void)snprintf(buf,cap,"%u",c->origin_auto); break;
    case 30: (void)snprintf(buf,cap,"%s",c->udp_bind); break;
    case 31: (void)snprintf(buf,cap,"%u",c->udp_port); break;
    case 32: (void)snprintf(buf,cap,"%u",c->udp_enabled); break;
    default: if(cap) buf[0]='\0'; break;
    }
}
static void render_editor(FILE *out,const editor *e,unsigned rows,unsigned columns)
{
    writer w={out,columns,0,0};
    unsigned i,first;
    unsigned visible=rows>10 ? rows-10 : 1;
    char value[2048];
    const char *display;
    if(visible>FIELD_COUNT) visible=FIELD_COUNT;
    /* Keep the selected field visible even in short terminals. Up/Down and
     * two-digit selection reach every appended setting without a fixed page. */
    first=(unsigned)e->selected>3 ? (unsigned)e->selected-3 : 0;
    if((unsigned)e->selected>=first+visible) first=(unsigned)e->selected-visible+1;
    if(first+visible>FIELD_COUNT) first=FIELD_COUNT-visible;
    write_text(&w,"[settings] Private draft. Active receiver remains unchanged.\n");
    write_text(&w,"%s%s\n",e->saved ? "SAVED - applies on restart. " : "",e->dirty ? "Unsaved draft edits." : "");
    write_text(&w,"Up/Down or number+Enter: edit | s: atomic save | Esc/q: back\n");
    for(i=first;i<FIELD_COUNT && i<first+visible;++i) {
        field_value(&e->draft,i,value,sizeof(value),0);
        write_text(&w,"%c %2u %-20s %.90s\n",i==(unsigned)e->selected ? '>' : ' ',i+1,fields[i],value);
    }
    if(e->editing) {
        if(e->selected==0 || e->selected==18) { cfg_safe_url(e->input,value,sizeof(value)); display=value; }
        else display=e->input;
        if(strlen(display)>80) display+=strlen(display)-80;
        write_text(&w,"Edit %s (ASCII; Enter validates, Esc cancels, Ctrl+U clears):\n> %s_\n",
                fields[e->selected],display);
        if(e->selected==0 || e->selected==18) write_text(&w,"URL query/fragment values stay hidden, including while editing.\n");
    } else if(e->number[0]) write_text(&w,"Selected field number: %s (Enter)\n",e->number);
    write_text(&w,"%s\n",e->notice);
}
static void editor_key(editor *e,int key,unsigned max_windows,const char *path)
{
    if(e->editing) {
        if(key==KEY_ESCAPE) { e->editing=0; notice(e,"Edit cancelled."); }
        else if(key==KEY_BACK) { if(e->input_n) e->input[--e->input_n]='\0'; }
        else if(key==KEY_CLEAR) { e->input_n=0; e->input[0]='\0'; }
        else if(key==KEY_ENTER) {
            tb_config next=e->draft;
            char error[160];
            if(cfg_set(&next,fields[e->selected],e->input,error,sizeof(error)) &&
               cfg_validate(&next,max_windows,error,sizeof(error))) {
                e->draft=next; e->dirty=1; e->editing=0;
                notice(e,"Draft validated. Press s to save; changes apply on restart.");
            } else notice(e,"Invalid setting: %s",error);
        } else if(key>=32 && key<=126 && e->input_n+1<sizeof(e->input)) {
            e->input[e->input_n++]=(char)key; e->input[e->input_n]='\0';
        }
        return;
    }
    if(key==KEY_ESCAPE || key=='q' || key=='Q') {
        e->open=0; e->draft=e->saved_config; e->dirty=0; e->number[0]='\0';
        notice(e,e->saved ? "SAVED - applies on restart. Active receiver continues." : "Settings closed. Receiver continues.");
    } else if(key==KEY_UP || key==KEY_DOWN) {
        e->selected=(e->selected+(key==KEY_UP ? FIELD_COUNT-1 : 1))%FIELD_COUNT;
        e->number[0]='\0';
    } else if(key>='0' && key<='9') {
        size_t len=strlen(e->number);
        if(len==2) len=0;
        e->number[len]=(char)key; e->number[len+1]='\0';
    } else if(key==KEY_ENTER) {
        if(e->number[0]) {
            int selected=atoi(e->number);
            e->number[0]='\0';
            if(selected<1 || selected>FIELD_COUNT) { notice(e,"Choose field 1-%d.",FIELD_COUNT); return; }
            e->selected=selected-1;
        }
        field_value(&e->draft,(unsigned)e->selected,e->input,sizeof(e->input),1);
        e->input_n=strlen(e->input); e->editing=1;
    } else if(key=='s' || key=='S') {
        char error[160];
        if(cfg_save(path,&e->draft,max_windows,error,sizeof(error))) {
            e->saved_config=e->draft; e->dirty=0; e->saved=1;
            notice(e,"SAVED - applies on restart. Current connection and pending data are unchanged.");
        } else notice(e,"Save failed: %s",error);
    }
}
static void draw(terminal *t,const tm_snapshot *s,const tb_config *c,const rates *r,const editor *e)
{
    unsigned rows=terminal_rows(),columns=t->ansi ? terminal_columns() : 0;
    unsigned visible=rows>22 ? rows-22 : 1;
    if(t->ansi) fputs("\033[?25l\033[H\033[2J",stderr);
    if(e->open) render_editor(stderr,e,rows,columns);
    else {
        render_snapshot(stderr,s,c,r,(unsigned)(e->show_devices ? e->device_scroll : e->scroll),
                        t->ansi ? visible : TB_MAX_KEYS,columns,e->show_devices,t->ansi);
        if(t->ansi) {
            writer w={stderr,columns,0,0};
            write_text(&w,"%s\n",t->input_ready ? "e: settings | d: devices | k: values | Up/Down scroll | q does not stop" :
                t->csv_console ? "Editing requires piped CSV input; keyboard belongs to CSV stdin." :
                "Keyboard unavailable; monitoring continues until input stops.");
            write_text(&w,"%s\n",e->notice);
        }
    }
    (void)fflush(stderr);
}
void tm_run(void *context,void (*snapshot)(void *,tm_snapshot *),const tb_config *active,const char *path)
{
    terminal term;
    editor edit;
    tm_snapshot current,previous;
    rates rate;
    uint64_t next,next_poll,now;
    unsigned refresh;
    int have_previous=0,redraw=0;
    if(!snapshot || !active) return;
    if(!path || !path[0]) path="telemetry.ini";
    terminal_open(&term,active);
    restore_install(&term);
    memset(&edit,0,sizeof(edit)); edit.draft=edit.saved_config=*active;
    edit.show_devices=strcmp(active->input_mode,"csv")!=0;
    memset(&previous,0,sizeof(previous)); memset(&current,0,sizeof(current));
    refresh=term.ansi ? active->refresh_ms : 1000;
    if(refresh<100) refresh=100;
    next=next_poll=mono_ms();
    for(;;) {
        int key=key_read(&term);
        now=mono_ms();
        if(key) {
            if(edit.open) editor_key(&edit,key,current.send_capacity ? current.send_capacity : 4,path);
            else if(key=='e' || key=='E') {
                edit.open=1; edit.editing=0; edit.draft=edit.saved_config; edit.number[0]='\0';
                notice(&edit,"Edit, validate, then save. Runtime settings stay fixed until restart.");
            } else if(key=='d' || key=='D') { edit.show_devices=1; }
            else if(key=='k' || key=='K') { edit.show_devices=0; }
            else if(key==KEY_UP) {
                if(edit.show_devices) { if(edit.device_scroll>0) --edit.device_scroll; }
                else if(edit.scroll>0) --edit.scroll;
            } else if(key==KEY_DOWN) {
                if(edit.show_devices) {
                    if((unsigned)(edit.device_scroll+1)<current.device_n &&
                       (unsigned)(edit.device_scroll+1)<TM_MAX_DEVICES) ++edit.device_scroll;
                } else if((unsigned)(edit.scroll+1)<current.key_n &&
                          (unsigned)(edit.scroll+1)<TB_MAX_KEYS) ++edit.scroll;
            } else if(key=='q' || key=='Q') notice(&edit,"Monitoring continues. Stop the input source or use configured duration to finish.");
            redraw=1;
        }
        /* Stop polling is independent of a possibly long display refresh. */
        if(now>=next_poll) {
            memset(&current,0,sizeof(current)); snapshot(context,&current);
            next_poll=now+250;
            if(current.stop) {
                compute_rates(&rate,&current,&previous,have_previous);
                edit.open=0; draw(&term,&current,active,&rate,&edit); break;
            }
        }
        if(now>=next) {
            compute_rates(&rate,&current,&previous,have_previous);
            draw(&term,&current,active,&rate,&edit);
            previous=current; have_previous=1; next=now+refresh; redraw=0;
        } else if(redraw && term.ansi && have_previous) {
            draw(&term,&current,active,&rate,&edit); redraw=0;
        }
        wait_tick();
    }
    if(term.ansi) { fputs("\033[0m\033[?25h\n",stderr); (void)fflush(stderr); }
    terminal_close(&term);
}
