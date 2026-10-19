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
enum { FIELD_COUNT=15 };
static const char *const fields[FIELD_COUNT]={
    "url","stream","session","token_env","origin_lat","origin_lon","origin_alt",
    "send_interval_ms","connect_timeout_ms","io_timeout_ms","max_windows",
    "refresh_ms","idle_warn_ms","ack_warn_ms","summary"
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
    int open, editing, selected, dirty, saved, scroll;
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
        (void)write(STDERR_FILENO,"\033[?25h",6);
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
    if(t->ansi) (void)write(STDERR_FILENO,"\033[?25h",6);
    t->ansi=0; t->input_ready=0;
}
#endif

static void terminal_open(terminal *t)
{
    memset(t,0,sizeof(*t));
#ifdef _WIN32
    t->input=INVALID_HANDLE_VALUE;
    t->output=GetStdHandle(STD_ERROR_HANDLE);
    if(!_isatty(_fileno(stderr)) || !GetConsoleMode(t->output,&t->old_output)) return;
    t->cursor_saved=GetConsoleCursorInfo(t->output,&t->cursor)!=0;
    if(!SetConsoleMode(t->output,t->old_output|ENABLE_VIRTUAL_TERMINAL_PROCESSING)) return;
    t->output_mode=1; t->ansi=1;
    if(_isatty(_fileno(stdin))) { t->csv_console=1; return; }
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
    if(isatty(fileno(stdin))) { t->csv_console=1; return; }
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
typedef struct { FILE *out; unsigned columns, used; } writer;
/* One bounded line at a time, never a full screen allocation. Console lines
 * are clipped before the last column so long keys/URLs cannot wrap the frame. */
static void write_text(writer *w,const char *fmt,...)
{
    char text[2304];
    const unsigned char *p;
    va_list ap;
    va_start(ap,fmt); (void)vsnprintf(text,sizeof(text),fmt,ap); va_end(ap);
    for(p=(const unsigned char *)text;*p;++p) {
        if(*p=='\n') { (void)fputc('\n',w->out); w->used=0; }
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
static void render_snapshot(FILE *out,const tm_snapshot *s,const tb_config *c,
                            const rates *r,unsigned first,unsigned max_keys,unsigned columns)
{
    writer w={out,columns,0};
    char endpoint[2048];
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
    write_text(&w,"[monitor] state=%s mode=%s uptime=%.1fs boundary=normalized-CSV\n",
            status(s),s->dry_run ? "DRY" : "HTTP",(double)elapsed(s->now_ms,s->started_ms)/1000);
    write_text(&w,"ACTIVE endpoint=%s\n",endpoint);
    write_text(&w,"ACTIVE stream=%.64s session=%.64s origin=(%.7g,%.7g,%.7g)\n",
            c->stream,c->session,c->origin[0],c->origin[1],c->origin[2]);
    write_text(&w,"ACTIVE interval=%ums connect=%ums io=%ums max_windows=%u summary=%d\n",
            c->send_interval_ms,c->connect_timeout_ms,c->io_timeout_ms,c->max_windows,c->summary);
    write_text(&w,"input bytes=%" PRIu64 " lines=%" PRIu64 " accepted=%" PRIu64 " rejected=%" PRIu64
            " rate=%.1f/s %.1fB/s\n",s->input_bytes,s->input_lines,s->accepted,s->rejected,r->accepted,r->bytes);
    write_text(&w,"loss gaps=%" PRIu64 " raw=%" PRIu64 " queue_windows=%" PRIu64 " queue_samples=%" PRIu64 "\n",
            s->sequence_gaps,s->raw_dropped,s->queue_windows_dropped,s->queue_samples_dropped);
    write_text(&w,"queue windows=%u/%u inflight=%u oldest=%" PRIu64 "ms current=%u raw=%u/%u\n",
            s->queue_count,s->queue_capacity,s->inflight_windows,s->oldest_age_ms,
            s->current_observations,s->current_raw,s->raw_capacity);
    write_text(&w,"event boot=%" PRIu64 "us watermark=%" PRIu64 "us open_window=%" PRIu64 "us\n",
            s->last_boot_us,s->watermark_us,s->current_window_us);
    write_text(&w,"network attempts=%" PRIu64 " acks=%" PRIu64 " failures=%" PRIu64 " dry_releases=%" PRIu64
            " (payload requests)\n",s->net_attempts,s->net_acks,s->net_failures,s->dry_releases);
    write_text(&w,"HTTP status=%ld error=%u consecutive=%u bytes_attempted=%" PRIu64
            " net_rtt=%.1fms encode=%.3fms\n",
            s->http_status,s->error_code,s->consecutive_failures,s->net_sent_bytes,s->last_rtt_ms,s->last_encode_ms);
    write_text(&w,"health=%s input_idle=%" PRIu64 "ms last_ack_age=%" PRIu64 "ms%s\n",
            health,idle,ack_age,!s->dry_run && s->queue_count && ack_age>=c->ack_warn_ms ? " WARN" : "");
    write_text(&w,"decoder=external CRC=n/a fix=n/a boundary=normalized-CSV\n");
    write_text(&w,"auth token_env=%.64s token=%s (credential value hidden)\n",
            c->token_env[0] ? c->token_env : "(none)",token && token[0] ? "set" : "missing");
    if(s->have_clock) {
        write_text(&w,"clock UTC=%s anchors=%u drift=%.3gppm resid=%.3gus\n",
                utc_status,(unsigned)s->clock.count,s->clock.drift_ppm,s->clock.residual_std_us);
        write_text(&w,"clock offset=%" PRId64 "us host_anchor_age=",s->clock.offset_us);
        if(s->last_anchor_receipt_ms) write_text(&w,"%" PRIu64 "ms\n",elapsed(s->now_ms,s->last_anchor_receipt_ms));
        else write_text(&w,"unknown\n");
    } else write_text(&w,"clock UTC=UNKNOWN anchors=0 (boot/session observations continue)\n");
    if(s->have_position) {
        write_text(&w,"ENU p=(%.3f,%.3f,%.3f)m v=(%.2f,%.2f,%.2f)m/s\n",
                s->p[0],s->p[1],s->p[2],(double)s->v[0],(double)s->v[1],(double)s->v[2]);
        write_text(&w,"alignment relative_uncert=%uus nav_age=%uus pos_error=%.3gm\n",
                (unsigned)s->time_uncert_us,(unsigned)s->nav_age_us,(double)s->pos_error_m);
        write_text(&w,"last_observation_UTC=");
        if(s->utc_us) write_text(&w,"%" PRIu64 "us absolute_uncert=%uus\n",s->utc_us,(unsigned)s->utc_uncert_us);
        else write_text(&w,"unknown absolute_uncert=n/a\n");
    } else write_text(&w,"ENU position=n/a alignment=n/a UTC=n/a\n");
    if(first>n) first=n;
    last=n-first>max_keys ? first+max_keys : n;
    write_text(&w,"keys %u-%u/%u (observations/s; Up/Down scroll)\n",n ? first+1 : 0,last,n);
    for(i=first;i<last;++i) {
        const tm_key *k=&s->keys[i];
        write_text(&w,"  %-20.*s %-8.*s rate=%6.1f n=%" PRIu64 " seq=%" PRIu32 " value=",
                TB_KEY_BYTES-1,k->key,TB_UNIT_BYTES-1,k->unit,r->key[i],k->observations,k->seq);
        if(k->have_value && isfinite(k->value)) write_text(&w,"%.9g",k->value);
        else write_text(&w,"n/a");
        write_text(&w," boot=%" PRIu64 "us\n",k->t_us);
    }
}
void tm_render(FILE *out,const tm_snapshot *s,const tb_config *c)
{
    rates r;
    if(!out || !s || !c) return;
    compute_rates(&r,s,s,0);
    render_snapshot(out,s,c,&r,0,TB_MAX_KEYS,0);
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
    default: (void)snprintf(buf,cap,"%d",c->summary); break;
    }
}
static void render_editor(FILE *out,const editor *e,unsigned rows,unsigned columns)
{
    writer w={out,columns,0};
    unsigned i,first=e->selected>3 ? (unsigned)e->selected-3 : 0;
    unsigned visible=rows>10 ? rows-10 : 4;
    char value[2048];
    const char *display;
    if(visible>FIELD_COUNT) visible=FIELD_COUNT;
    if(first+visible>FIELD_COUNT) first=FIELD_COUNT-visible;
    write_text(&w,"[settings] Private draft. Active receiver remains unchanged.\n");
    write_text(&w,"%s%s\n",e->saved ? "SAVED - applies on restart. " : "",e->dirty ? "Unsaved draft edits." : "");
    write_text(&w,"Up/Down or number+Enter: edit | s: atomic save | Esc/q: back\n");
    for(i=first;i<FIELD_COUNT && i<first+visible;++i) {
        field_value(&e->draft,i,value,sizeof(value),0);
        write_text(&w,"%c %2u %-20s %.90s\n",i==(unsigned)e->selected ? '>' : ' ',i+1,fields[i],value);
    }
    if(e->editing) {
        if(e->selected==0) { cfg_safe_url(e->input,value,sizeof(value)); display=value; }
        else display=e->input;
        if(strlen(display)>80) display+=strlen(display)-80;
        write_text(&w,"Edit %s (ASCII; Enter validates, Esc cancels, Ctrl+U clears):\n> %s_\n",
                fields[e->selected],display);
        if(e->selected==0) write_text(&w,"URL query/fragment values stay hidden, including while editing.\n");
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
        render_snapshot(stderr,s,c,r,(unsigned)e->scroll,t->ansi ? visible : TB_MAX_KEYS,columns);
        if(t->ansi) {
            writer w={stderr,columns,0};
            write_text(&w,"%s\n",t->input_ready ? "e: settings | Up/Down: keys | CSV EOF ends receiver; q does not stop it" :
                t->csv_console ? "Editing requires piped CSV input; keyboard belongs to CSV stdin." :
                "Keyboard unavailable; monitoring continues until CSV EOF.");
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
    terminal_open(&term);
    restore_install(&term);
    memset(&edit,0,sizeof(edit)); edit.draft=edit.saved_config=*active;
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
            } else if(key==KEY_UP) { if(edit.scroll>0) --edit.scroll; }
            else if(key==KEY_DOWN) {
                if((unsigned)(edit.scroll+1)<current.key_n && (unsigned)(edit.scroll+1)<TB_MAX_KEYS) ++edit.scroll;
            } else if(key=='q' || key=='Q') notice(&edit,"Monitoring continues. Finish the CSV source to stop the receiver.");
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
    if(term.ansi) { fputs("\033[?25h\n",stderr); (void)fflush(stderr); }
    terminal_close(&term);
}
