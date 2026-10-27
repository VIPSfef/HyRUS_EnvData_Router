/* Optional loopback diagnostic HTTP server. Fixed storage; no observation files.
 * Parsing is O(header bytes + JSON bytes), nesting <=32, tokens <=32768.
 * Each accepted connection has a 5 s total deadline and <=100 ms polls.
 * The single diagnostic connection is deliberately isolated from the router's
 * receive/HTTP worker threads. 204 means fully validated and printed, not disk
 * persistence. This executable is never part of the ordinary four app builds. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "monitor_frame.h"
#include "udp_backup.h"
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tb_version.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <io.h>
typedef SOCKET ts_socket;
#define TS_INVALID INVALID_SOCKET
#define ts_close closesocket
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
typedef int ts_socket;
#define TS_INVALID (-1)
#define ts_close close
#endif

#define TS_HEADER_CAP 8192u
#define TS_BODY_CAP (512u*1024u)
#define TS_TOKEN_CAP 32768u
#define TS_LOG_CAP 8u
#define TS_RECORD_CAP 64u
#define TS_NONE UINT32_MAX
enum { TS_REFRESH_MS=250, TS_STATUS_DEVICE_CAP=32, TS_KEY_CAP=64,
       TS_KEY_VISIBLE=12, TS_ID_BITS=256*256, TS_ID_BYTES=TS_ID_BITS/8,
       TS_RECENT_CAP=8, TS_STREAM_CAP=256 };
typedef enum { TS_METRIC_TEMPERATURE, TS_METRIC_HUMIDITY, TS_METRIC_COUNT } ts_metric_kind;
typedef enum { TS_HTTP,TS_UDP } ts_transport;
static const char *const metric_fields[TS_METRIC_COUNT]={"temperature","humidity"};

typedef struct { uint32_t start,end,next; char kind; } ts_token;
typedef struct { const char *text; size_t size,pos; unsigned count; ts_token token[TS_TOKEN_CAP]; } ts_json;
typedef struct { char text[192]; } ts_log;
typedef struct {
    double value;
    uint64_t received_ms,source_age_ms;
    int known,age_known;
} ts_metric;
typedef struct {
    unsigned sysid,compid,message,sensor,instance;
    ts_metric_kind metric;
    double value;
    uint64_t seq,received_ms,observations;
} ts_key;
typedef struct {
    double latitude,longitude,altitude;
    uint64_t received_ms,source_age_ms;
    unsigned fix;
    int known,age_known;
    char source[8],state[8];
} ts_position;
/* Exact received bytes, not a hash: matching immutable IDs with different
 * payloads must not disappear as a backup duplicate. This bounded diagnostic
 * cache only skips a delivery arriving through the other transport. */
typedef struct {
    size_t length,stream_length;
    unsigned schema;
    ts_transport transport;
    char stream[TS_STREAM_CAP];
    char record[TS_BODY_CAP];
} ts_recent;
typedef struct {
    uint64_t started,end_ms,requests,accepted,records,statuses,rejected,max_requests;
    uint64_t order,last_received_ms,status_received_ms;
    unsigned port,ansi,log_n,log_next;
    unsigned udp_port,udp_error,recent_next;
    uint64_t udp_messages,udp_accepted,udp_rejected,cross_duplicates;
    ub_receiver *udp;
    ts_transport transport;
    ts_recent recent[TS_RECENT_CAP];
    unsigned key_n,id_n,keys_overflow,link_sysid,link_compid;
    int have_status,connected;
    char bind[64];
    char link_state[32];
    unsigned char ids[TS_ID_BYTES];
    ts_key keys[TS_KEY_CAP];
    ts_metric metrics[TS_METRIC_COUNT];
    ts_position position;
    ts_log log[TS_LOG_CAP];
    char request[TS_HEADER_CAP+TS_BODY_CAP+1u];
    ts_json json;
} ts_server;
static ts_server server;
static volatile sig_atomic_t stop_requested;
static int dashboard(int force);
static void udp_message(void *context,const char *body,size_t length);

static uint64_t now_ms(void) {
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec t;
    if(clock_gettime(CLOCK_MONOTONIC,&t)) return 0;
    return (uint64_t)t.tv_sec*1000u+(uint64_t)t.tv_nsec/1000000u;
#endif
}
static int stopped(void) { return stop_requested || (server.end_ms && now_ms()>=server.end_ms); }
#ifdef _WIN32
static BOOL WINAPI stop_console(DWORD event) {
    if(event==CTRL_C_EVENT || event==CTRL_BREAK_EVENT || event==CTRL_CLOSE_EVENT) { stop_requested=1; return TRUE; }
    return FALSE;
}
#else
static void stop_signal(int event) { (void)event; stop_requested=1; }
#endif
static int would_block(void) {
#ifdef _WIN32
    int e=WSAGetLastError(); return e==WSAEWOULDBLOCK || e==WSAEINTR;
#else
    return errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR;
#endif
}
static int nonblock(ts_socket socket_fd) {
#ifdef _WIN32
    u_long mode=1; return ioctlsocket(socket_fd,FIONBIO,&mode)==0;
#else
    int flags=fcntl(socket_fd,F_GETFL,0); return flags>=0 && !fcntl(socket_fd,F_SETFL,flags|O_NONBLOCK);
#endif
}
/* 1 ready, 0 timeout/interrupted, -1 error. Never holds shutdown over 100 ms. */
static int socket_ready(ts_socket socket_fd,int writing) {
    fd_set set;
    struct timeval tv={0,100000};
    int rc;
#ifndef _WIN32
    if(socket_fd<0 || socket_fd>=FD_SETSIZE) return -1;
#endif
    if(server.udp) ub_receiver_poll(server.udp,now_ms());
    FD_ZERO(&set); FD_SET(socket_fd,&set);
    rc=select((int)(socket_fd+1),writing ? NULL : &set,writing ? &set : NULL,NULL,&tv);
    if(rc<0 && would_block()) rc=0;
    if(server.udp) ub_receiver_poll(server.udp,now_ms());
    (void)dashboard(0);
    return rc;
}
static int send_all(ts_socket client,const char *data,size_t length,uint64_t deadline) {
    size_t sent=0;
    while(sent<length && !stopped() && now_ms()<deadline) {
        int ready=socket_ready(client,1),n;
        if(ready<0) return 0;
        if(!ready) continue;
#ifdef MSG_NOSIGNAL
        n=(int)send(client,data+sent,length-sent,MSG_NOSIGNAL);
#else
        n=(int)send(client,data+sent,(int)(length-sent),0);
#endif
        if(n>0) sent+=(size_t)n;
        else if(!n || !would_block()) return 0;
    }
    return sent==length;
}
static void reply(ts_socket client,unsigned code,uint64_t deadline) {
    char response[192];
    const char *reason=code==204 ? "No Content" : code==404 ? "Not Found" : code==411 ? "Length Required" :
                       code==413 ? "Content Too Large" : code==431 ? "Request Header Fields Too Large" :
                       code==500 ? "Internal Server Error" : "Bad Request";
    int n=snprintf(response,sizeof(response),"HTTP/1.1 %u %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",code,reason);
    if(n>0 && (size_t)n<sizeof(response)) (void)send_all(client,response,(size_t)n,deadline);
}
static int equal_ascii(const char *text,size_t size,const char *word) {
    size_t i;
    if(size!=strlen(word)) return 0;
    for(i=0;i<size;++i) if(tolower((unsigned char)text[i])!=tolower((unsigned char)word[i])) return 0;
    return 1;
}
static int unsigned_number(const char *text,size_t size,uint64_t *value) {
    uint64_t n=0; size_t i;
    if(!size) return 0;
    for(i=0;i<size;++i) {
        unsigned digit=(unsigned char)text[i]-'0';
        if(digit>9 || n>(UINT64_MAX-digit)/10u) return 0;
        n=n*10u+digit;
    }
    *value=n; return 1;
}
/* Header parser never mutates the bytes already received after CRLFCRLF. */
static unsigned parse_headers(const char *data,size_t end,size_t *body_size,int *expect_continue) {
    size_t pos=0,line_end,space1,space2;
    int have_length=0;
    *body_size=0; *expect_continue=0;
    while(pos+1<end && !(data[pos]=='\r' && data[pos+1]=='\n')) ++pos;
    if(pos+1>=end) return 400;
    line_end=pos; space1=0;
    while(space1<line_end && data[space1]!=' ') ++space1;
    space2=space1+1;
    while(space2<line_end && data[space2]!=' ') ++space2;
    if(space1!=4 || memcmp(data,"POST",4) || space2<=space1+1 ||
       space2+9!=line_end || (memcmp(data+space2+1,"HTTP/1.1",8) && memcmp(data+space2+1,"HTTP/1.0",8))) return 400;
    for(size_t i=space1+1;i<space2;++i) if((unsigned char)data[i]<=32 || (unsigned char)data[i]>=127) return 400;
    if(space2-space1-1!=7 || memcmp(data+space1+1,"/ingest",7)) return 404;
    pos+=2;
    while(pos+1<end) {
        size_t first=pos,colon,value,last;
        if(data[pos]=='\r' && data[pos+1]=='\n') return have_length ? 0 : 411;
        while(pos+1<end && !(data[pos]=='\r' && data[pos+1]=='\n')) ++pos;
        if(pos+1>=end) return 400;
        line_end=pos; colon=first;
        while(colon<line_end && data[colon]!=':') {
            unsigned char c=(unsigned char)data[colon];
            if(!c || (!isalnum(c) && !strchr("!#$%&'*+-.^_`|~",c))) return 400;
            ++colon;
        }
        if(colon==first || colon==line_end) return 400;
        value=colon+1; last=line_end;
        while(value<last && (data[value]==' ' || data[value]=='\t')) ++value;
        while(last>value && (data[last-1]==' ' || data[last-1]=='\t')) --last;
        for(size_t i=value;i<last;++i) if(((unsigned char)data[i]<32 && data[i]!='\t') || (unsigned char)data[i]==127) return 400;
        if(equal_ascii(data+first,colon-first,"Content-Length")) {
            uint64_t n;
            if(have_length || !unsigned_number(data+value,last-value,&n)) return 400;
            if(n>TS_BODY_CAP) return 413;
            *body_size=(size_t)n; have_length=1;
        } else if(equal_ascii(data+first,colon-first,"Transfer-Encoding")) return 400;
        else if(equal_ascii(data+first,colon-first,"Expect")) {
            if(!equal_ascii(data+value,last-value,"100-continue") || *expect_continue) return 400;
            *expect_continue=1;
        }
        pos=line_end+2;
    }
    return 400;
}

static void whitespace(ts_json *j) {
    while(j->pos<j->size && (j->text[j->pos]==' ' || j->text[j->pos]=='\r' ||
          j->text[j->pos]=='\n' || j->text[j->pos]=='\t')) ++j->pos;
}
static int hex_digit(unsigned char c) { return (c>='0' && c<='9') || (c>='a' && c<='f') || (c>='A' && c<='F'); }
/* Reject invalid UTF-8, overlong encodings, surrogate values and >U+10FFFF. */
static unsigned utf8_width(const unsigned char *s,size_t size) {
    unsigned n;
    if(!size || s[0]<0xc2 || s[0]>0xf4) return 0;
    n=s[0]<0xe0 ? 2u : s[0]<0xf0 ? 3u : 4u;
    if(size<n) return 0;
    for(unsigned i=1;i<n;++i) if((s[i]&0xc0)!=0x80) return 0;
    if((s[0]==0xe0 && s[1]<0xa0) || (s[0]==0xed && s[1]>=0xa0) ||
       (s[0]==0xf0 && s[1]<0x90) || (s[0]==0xf4 && s[1]>=0x90)) return 0;
    return n;
}
static int parse_string(ts_json *j) {
    if(j->pos>=j->size || j->text[j->pos++]!='"') return 0;
    while(j->pos<j->size) {
        unsigned char c=(unsigned char)j->text[j->pos++];
        if(c=='"') return 1;
        if(c<32) return 0;
        if(c=='\\') {
            if(j->pos>=j->size) return 0;
            c=(unsigned char)j->text[j->pos++];
            if(c=='u') {
                if(j->size-j->pos<4) return 0;
                for(unsigned i=0;i<4;++i) if(!hex_digit((unsigned char)j->text[j->pos+i])) return 0;
                j->pos+=4;
            } else if(!c || !strchr("\"\\/bfnrt",c)) return 0;
        } else if(c>=128) {
            unsigned n=utf8_width((const unsigned char *)j->text+j->pos-1,j->size-j->pos+1);
            if(!n) return 0;
            j->pos+=n-1;
        }
    }
    return 0;
}
static int parse_number(ts_json *j) {
    size_t start=j->pos;
    if(j->pos<j->size && j->text[j->pos]=='-') ++j->pos;
    if(j->pos>=j->size) return 0;
    if(j->text[j->pos]=='0') ++j->pos;
    else {
        if(j->text[j->pos]<'1' || j->text[j->pos]>'9') return 0;
        while(j->pos<j->size && j->text[j->pos]>='0' && j->text[j->pos]<='9') ++j->pos;
    }
    if(j->pos<j->size && j->text[j->pos]=='.') {
        size_t first=++j->pos;
        while(j->pos<j->size && j->text[j->pos]>='0' && j->text[j->pos]<='9') ++j->pos;
        if(first==j->pos) return 0;
    }
    if(j->pos<j->size && (j->text[j->pos]=='e' || j->text[j->pos]=='E')) {
        size_t first;
        ++j->pos;
        if(j->pos<j->size && (j->text[j->pos]=='+' || j->text[j->pos]=='-')) ++j->pos;
        first=j->pos;
        while(j->pos<j->size && j->text[j->pos]>='0' && j->text[j->pos]<='9') ++j->pos;
        if(first==j->pos) return 0;
    }
    return j->pos>start;
}
static unsigned parse_value(ts_json *j,unsigned depth) {
    unsigned token; char c;
    whitespace(j);
    if(depth>32 || j->pos>=j->size || j->count>=TS_TOKEN_CAP) return TS_NONE;
    token=j->count++; j->token[token].start=(uint32_t)j->pos;
    c=j->text[j->pos]; j->token[token].kind=c;
    if(c=='{' || c=='[') {
        char close=c=='{' ? '}' : ']';
        ++j->pos; whitespace(j);
        if(j->pos<j->size && j->text[j->pos]==close) ++j->pos;
        else for(;;) {
            if(c=='{') {
                unsigned key=parse_value(j,depth+1);
                if(key==TS_NONE || j->token[key].kind!='"') return TS_NONE;
                whitespace(j); if(j->pos>=j->size || j->text[j->pos++]!=':') return TS_NONE;
            }
            if(parse_value(j,depth+1)==TS_NONE) return TS_NONE;
            whitespace(j);
            if(j->pos>=j->size) return TS_NONE;
            if(j->text[j->pos]==close) { ++j->pos; break; }
            if(j->text[j->pos++]!=',') return TS_NONE;
        }
    } else if(c=='"') { if(!parse_string(j)) return TS_NONE; }
    else if(c=='t' || c=='f' || c=='n') {
        const char *word=c=='t' ? "true" : c=='f' ? "false" : "null";
        size_t n=strlen(word);
        if(j->size-j->pos<n || memcmp(j->text+j->pos,word,n)) return TS_NONE;
        j->pos+=n;
    } else if(!parse_number(j)) return TS_NONE;
    j->token[token].end=(uint32_t)j->pos; j->token[token].next=j->count;
    return token;
}
static int token_text(const ts_json *j,unsigned token,const char *text) {
    const ts_token *t;
    size_t n=strlen(text);
    if(token>=j->count) return 0;
    t=&j->token[token];
    return t->kind=='"' && t->end-t->start==n+2 && !memcmp(j->text+t->start+1,text,n);
}
/* Repeated known fields are rejected rather than silently choosing a value. */
static unsigned field(const ts_json *j,unsigned object,const char *name) {
    unsigned value=TS_NONE,i;
    if(object>=j->count || j->token[object].kind!='{') return TS_NONE;
    for(i=object+1;i<j->token[object].next;) {
        unsigned next=i+1;
        if(next>=j->count) return TS_NONE;
        if(token_text(j,i,name)) { if(value!=TS_NONE) return TS_NONE; value=next; }
        i=j->token[next].next;
    }
    return value;
}
static int token_uint(const ts_json *j,unsigned token,uint64_t *value) {
    const ts_token *t;
    if(token>=j->count) return 0;
    t=&j->token[token];
    return t->kind>='0' && t->kind<='9' && unsigned_number(j->text+t->start,t->end-t->start,value);
}
static int token_double(const ts_json *j,unsigned token,double *value) {
    char buf[80],*end;
    size_t n;
    if(token>=j->count || (j->token[token].kind!='-' && (j->token[token].kind<'0' || j->token[token].kind>'9'))) return 0;
    n=j->token[token].end-j->token[token].start;
    if(n>=sizeof(buf)) return 0;
    memcpy(buf,j->text+j->token[token].start,n); buf[n]=0;
    errno=0; *value=strtod(buf,&end);
    return !errno && end==buf+n && isfinite(*value);
}
static int validate_record(const ts_json *j,unsigned record) {
    static const char *const hb_fields[]={"sysid","compid","type","autopilot","base_mode","custom_mode","system_status","received_monotonic_ms"};
    unsigned heartbeat=field(j,record,"heartbeat"),position=field(j,record,"position");
    unsigned source=field(j,record,"source"),session=field(j,record,"session"),age=field(j,record,"coordinate_age_ms");
    uint64_t value;
    if(!token_uint(j,field(j,record,"seq"),&value) ||
       !token_uint(j,field(j,record,"source_generation"),&value) ||
       source==TS_NONE || j->token[source].kind!='"' || j->token[source].end<=j->token[source].start+2 ||
       session==TS_NONE || j->token[session].kind!='"' || j->token[session].end<=j->token[session].start+2 ||
       heartbeat==TS_NONE || j->token[heartbeat].kind!='{' || position==TS_NONE || age==TS_NONE ||
       (j->token[age].kind!='n' && !token_uint(j,age,&value))) return 0;
    for(unsigned i=0;i<sizeof(hb_fields)/sizeof(hb_fields[0]);++i) {
        if(!token_uint(j,field(j,heartbeat,hb_fields[i]),&value)) return 0;
        if(i!=7 && value>(i==5 ? UINT32_MAX : 255u)) return 0;
    }
    if(j->token[position].kind!='n') {
        double lat,lon,alt;
        if(!token_double(j,field(j,position,"lat_deg"),&lat) || lat < -90 || lat > 90 ||
           !token_double(j,field(j,position,"lon_deg"),&lon) || lon < -180 || lon > 180 ||
           !token_double(j,field(j,position,"alt_msl_m"),&alt) ||
           !token_uint(j,field(j,position,"boot_ms"),&value) ||
           !token_uint(j,field(j,position,"received_monotonic_ms"),&value)) return 0;
    }
    return 1;
}
static int token_u64_string(const ts_json *j,unsigned token,uint64_t *value) {
    const ts_token *t;
    if(token>=j->count) return 0;
    t=&j->token[token];
    return t->kind=='"' && t->end>t->start+2 &&
           unsigned_number(j->text+t->start+1,t->end-t->start-2,value);
}
static int nonempty_string(const ts_json *j,unsigned token) {
    return token<j->count && j->token[token].kind=='"' &&
           j->token[token].end>j->token[token].start+2;
}
static int null_token(const ts_json *j,unsigned token) {
    return token<j->count && j->token[token].kind=='n';
}
static int nullable_nonnegative(const ts_json *j,unsigned token) {
    double value;
    return null_token(j,token) || (token_double(j,token,&value) && value>=0.0);
}
/* A status is an observation of receiver state, never another measurement.
 * Freshness has its own nullable monotonic age; elapsed runtime cannot stand in
 * for an age when a heartbeat or weather value has never been observed. */
static int status_age(const ts_json *j,unsigned object,const char *state_key,
                      const char *age_key,const char *waiting,const char *live,const char *old) {
    unsigned state=field(j,object,state_key),age=field(j,object,age_key);
    uint64_t value;
    if(token_text(j,state,waiting)) return null_token(j,age);
    return (token_text(j,state,live) || token_text(j,state,old)) && token_u64_string(j,age,&value);
}
static int status_link(const ts_json *j,unsigned object,int device) {
    uint64_t sysid,compid;
    return token_uint(j,field(j,object,"sysid"),&sysid) && sysid<=255 && (!device || sysid) &&
           token_uint(j,field(j,object,"compid"),&compid) && compid<=255 && (!device || compid) &&
           status_age(j,object,"heartbeat","hb_age_ms","WAIT","LIVE","LOST") &&
           status_age(j,object,"data","data_age_ms","WAIT","LIVE","STALE");
}
static int status_weather(const ts_json *j,unsigned object,int humidity) {
    unsigned state=field(j,object,"state"),value=field(j,object,"value");
    uint64_t observations;
    double number;
    if(!token_text(j,field(j,object,"unit"),humidity ? "1" : "K") ||
       !token_u64_string(j,field(j,object,"observations"),&observations) ||
       !status_age(j,object,"state","rx_age_ms","UNKNOWN","FRESH","STALE")) return 0;
    if(token_text(j,state,"UNKNOWN")) return null_token(j,value) && !observations;
    return observations && token_double(j,value,&number) && number>=0 && (!humidity || number<=1);
}
static int status_position(const ts_json *j,unsigned object) {
    double latitude,longitude,altitude;
    uint64_t number;
    unsigned timeout=field(j,object,"stale_after_ms");
    if(object==TS_NONE || null_token(j,object)) return 1;
    return (token_text(j,field(j,object,"source"),"GPS") || token_text(j,field(j,object,"source"),"ESTGPS")) &&
        (token_text(j,field(j,object,"state"),"LIVE") || token_text(j,field(j,object,"state"),"STALE") ||
         token_text(j,field(j,object,"state"),"INVALID")) &&
        token_uint(j,field(j,object,"fix"),&number) && number<=255 &&
        token_double(j,field(j,object,"lat_deg"),&latitude) && latitude>=-90 && latitude<=90 &&
        token_double(j,field(j,object,"lon_deg"),&longitude) && longitude>=-180 && longitude<=180 &&
        token_double(j,field(j,object,"alt_msl_m"),&altitude) &&
        token_u64_string(j,field(j,object,"rx_age_ms"),&number) &&
        (timeout==TS_NONE || (token_uint(j,timeout,&number) && number && number<=UINT32_MAX));
}
static int status_record(const ts_json *j,unsigned record) {
    static const char *const counters[]={"seq","generation","reported_monotonic_ms","run_elapsed_ms"};
    static const char *const packets[]={"received","estimated_lost","duplicates"};
    unsigned source=field(j,record,"source"),weather=field(j,record,"weather");
    unsigned packet=field(j,record,"packets"),devices=field(j,record,"devices");
    unsigned endpoint=field(j,source,"endpoint"),state=field(j,source,"state");
    unsigned connected=field(j,source,"connected"),count=0;
    uint64_t number,ids[TS_STATUS_DEVICE_CAP];
    if(!nonempty_string(j,field(j,record,"session")) ||
       !token_uint(j,field(j,record,"sample_count"),&number) || number ||
       !status_link(j,source,0) || endpoint>=j->count || j->token[endpoint].kind!='"' ||
       state>=j->count || j->token[state].kind!='"' ||
       connected>=j->count || (j->token[connected].kind!='t' && j->token[connected].kind!='f') ||
       ((j->token[connected].kind=='t')!=(token_text(j,state,"LOCKED") &&
          token_text(j,field(j,source,"heartbeat"),"LIVE"))) ||
       !token_uint(j,field(j,source,"serial_baud"),&number) || number>UINT32_MAX ||
       !status_weather(j,field(j,weather,"temperature"),0) ||
       !status_weather(j,field(j,weather,"humidity"),1) ||
       !status_position(j,field(j,record,"position")) ||
       devices>=j->count || j->token[devices].kind!='[') return 0;
    for(unsigned i=0;i<sizeof(counters)/sizeof(counters[0]);++i)
        if(!token_u64_string(j,field(j,record,counters[i]),&number)) return 0;
    for(unsigned i=0;i<sizeof(packets)/sizeof(packets[0]);++i)
        if(!token_u64_string(j,field(j,packet,packets[i]),&number)) return 0;
    for(unsigned device=devices+1;device<j->token[devices].next;device=j->token[device].next) {
        uint64_t sysid=0,compid=0,id;
        if(count>=TS_STATUS_DEVICE_CAP || !status_link(j,device,1) ||
           !token_u64_string(j,field(j,device,"rx_age_ms"),&number)) return 0;
        (void)token_uint(j,field(j,device,"sysid"),&sysid);
        (void)token_uint(j,field(j,device,"compid"),&compid);
        id=(sysid<<8)|compid;
        for(unsigned previous=0;previous<count;++previous) if(ids[previous]==id) return 0;
        ids[count++]=id;
    }
    return 1;
}
static int vector3(const ts_json *j,unsigned token) {
    unsigned i,n=0; double value;
    if(token>=j->count || j->token[token].kind!='[') return 0;
    for(i=token+1;i<j->token[token].next;i=j->token[i].next)
        if(++n>3 || !token_double(j,i,&value)) return 0;
    return n==3;
}
static int weather_sample_record(const ts_json *j,unsigned sample,int humidity) {
    unsigned t=field(j,sample,"t_us"),p=field(j,sample,"position_m"),v=field(j,sample,"velocity_mps");
    unsigned tu=field(j,sample,"time_uncertainty_us"),nu=field(j,sample,"navigation_uncertainty_m");
    unsigned pu=field(j,sample,"position_uncertainty_m");
    uint64_t number,quality; double value;
    int known_time=!null_token(j,t),known_position=!null_token(j,p);
    if(!token_double(j,field(j,sample,"value"),&value) || value<0 || (humidity && value>1) ||
       !nullable_nonnegative(j,field(j,sample,"measurement_variance")) ||
       !nullable_nonnegative(j,nu) || !nullable_nonnegative(j,pu) ||
       !token_uint(j,field(j,sample,"quality"),&quality) || quality>UINT32_MAX ||
       !token_uint(j,field(j,sample,"seq"),&number) || number>UINT32_MAX ||
       (known_time && !token_u64_string(j,t,&number)) ||
       (!null_token(j,tu) && (!token_uint(j,tu,&number) || number>=UINT32_MAX))) return 0;
    if(known_position) {
        if(!known_time || !vector3(j,p) || !vector3(j,v)) return 0;
    } else if(!null_token(j,v) || !null_token(j,nu) || !null_token(j,pu)) return 0;
    if(!known_time && !null_token(j,tu)) return 0;
    if(((quality&UINT32_C(0x08000000))!=0)==known_time ||
       ((quality&UINT32_C(0x04000000))!=0)==known_position) return 0;
    {
        unsigned source=field(j,sample,"position_source"),age=field(j,sample,"estimation_age_us");
        if(source<j->count) {
            int gps=token_text(j,source,"GPS"),estimated=token_text(j,source,"ESTGPS");
            if(!gps && !estimated && !token_text(j,source,"UNKNOWN")) return 0;
            if((gps || estimated) && !known_position) return 0;
            if(((quality&UINT32_C(0x10000000))!=0)!=estimated) return 0;
            if(!null_token(j,age) && (!token_uint(j,age,&number) || number>=UINT32_MAX)) return 0;
            if(gps && (null_token(j,age) || number!=0)) return 0;
            if(!gps && !estimated && !null_token(j,age)) return 0;
        } else if(age<j->count) return 0;
    }
    {
        unsigned valid=field(j,sample,"velocity_valid"),segment=field(j,sample,"navigation_segment");
        if(valid<j->count && ((j->token[valid].kind!='t' && j->token[valid].kind!='f') ||
           (j->token[valid].kind=='t' && !known_position))) return 0;
        if(segment<j->count && (!token_uint(j,segment,&number) || number>UINT32_MAX)) return 0;
    }
    return 1;
}
static int spatial_record(const ts_json *j,unsigned object,unsigned *count) {
    unsigned bins=field(j,object,"bins"),adaptive=field(j,object,"adaptive");
    double target; uint64_t number;
    *count=0;
    if(!token_text(j,field(j,object,"schema"),"mavlink_spatial_v1") ||
       !token_uint(j,field(j,object,"si_registry"),&number) || number!=1 ||
       !token_double(j,field(j,object,"target_step_m"),&target) || target<=0 ||
       adaptive>=j->count || (j->token[adaptive].kind!='t' && j->token[adaptive].kind!='f') ||
       !token_u64_string(j,field(j,object,"gap_us"),&number) || !number ||
       bins>=j->count || j->token[bins].kind!='[') return 0;
    for(unsigned b=bins+1;b<j->token[bins].next;b=j->token[b].next) {
        unsigned stats=field(j,b,"statistics"),rep=field(j,b,"representative");
        int humidity=token_text(j,field(j,b,"metric"),"humidity");
        uint64_t n,start,end,stamp;
        double mean,m2,variance,min,max,width,left,right,first,last,entry,value;
        if(++*count>144 || (!humidity && !token_text(j,field(j,b,"metric"),"temperature")) ||
           !token_u64_string(j,field(j,b,"continuity_key"),&number) ||
           !token_u64_string(j,field(j,b,"path_id"),&number) || !number ||
           !token_u64_string(j,field(j,b,"bin_index"),&number) ||
           !token_u64_string(j,field(j,b,"revision"),&number) || !number ||
           (!token_text(j,field(j,b,"state"),"partial") &&
            !token_text(j,field(j,b,"state"),"complete") && !token_text(j,field(j,b,"state"),"interrupted")) ||
           !token_uint(j,field(j,b,"message_id"),&number) || number>UINT32_C(0xffffff) ||
           !token_uint(j,field(j,b,"sensor_id"),&number) || number>UINT32_MAX ||
           !token_uint(j,field(j,b,"sensor_instance"),&number) || number>255 ||
           !token_uint(j,field(j,b,"flags"),&number) || number>UINT32_MAX ||
           !token_uint(j,field(j,b,"quality"),&number) || number>UINT32_MAX ||
           !weather_sample_record(j,rep,humidity) ||
           !token_u64_string(j,field(j,rep,"t_us"),&stamp) ||
           !token_double(j,field(j,rep,"value"),&value) ||
           !token_u64_string(j,field(j,b,"t_start_us"),&start) || !start ||
           !token_u64_string(j,field(j,b,"t_end_us"),&end) || end<start || stamp<start || stamp>end ||
           !token_double(j,field(j,b,"width_m"),&width) || width<target ||
           !token_double(j,field(j,b,"interval_start_m"),&left) || left<0 ||
           !token_double(j,field(j,b,"interval_end_m"),&right) || right<=left ||
           fabs((right-left)-width)>1e-7*fmax(1.0,width) ||
           !token_double(j,field(j,b,"observed_start_m"),&first) || first<left-1e-6 ||
           !token_double(j,field(j,b,"observed_end_m"),&last) || last<first || last>right+1e-6 ||
           !token_double(j,field(j,b,"entry_step_m"),&entry) || entry<0 ||
           !token_uint(j,field(j,stats,"n"),&n) || !n || n>UINT32_MAX ||
           !token_double(j,field(j,stats,"mean"),&mean) ||
           !token_double(j,field(j,stats,"m2"),&m2) || m2<0 ||
           !token_double(j,field(j,stats,"min"),&min) || min<0 ||
           !token_double(j,field(j,stats,"max"),&max) || max<min ||
           mean<min || mean>max || value<min || value>max || (humidity && max>1) ||
           (null_token(j,field(j,stats,"variance"))!=(n==1))) return 0;
        if(n>1 && (!token_double(j,field(j,stats,"variance"),&variance) || variance<0 ||
           fabs(variance-m2/(double)(n-1))>1e-9*fmax(1.0,variance))) return 0;
        if(token_text(j,field(j,b,"source_kind"),"GPS")) {
            if(!token_text(j,field(j,rep,"position_source"),"GPS")) return 0;
        } else if(!token_text(j,field(j,b,"source_kind"),"ESTGPS") ||
                  !token_text(j,field(j,rep,"position_source"),"ESTGPS")) return 0;
    }
    return 1;
}
static int weather_body_record(const ts_json *j,unsigned record) {
    unsigned weather=field(j,record,"weather"),source=field(j,record,"source");
    unsigned measurements=field(j,weather,"measurements"),groups=0,spatial_n=0;
    unsigned spatial=field(j,record,"spatial");
    uint64_t value,total_n,total_raw,total_dropped,sum_n=0,sum_raw=0,sum_dropped=0;
    uint64_t ids[16],messages[16]; unsigned instances[16],metrics[16];
    if(spatial<j->count && !spatial_record(j,spatial,&spatial_n)) return 0;
    if(!nonempty_string(j,field(j,record,"session")) ||
       !token_uint(j,field(j,record,"generation"),&value) ||
       !token_uint(j,field(j,record,"seq"),&value) ||
       !token_uint(j,field(j,source,"sysid"),&value) || !value || value>255 ||
       !token_uint(j,field(j,source,"compid"),&value) || value>255 ||
       !token_uint(j,field(j,weather,"si_registry"),&value) || value!=1 ||
       !token_u64_string(j,field(j,weather,"host_window_ms"),&value) ||
       !token_uint(j,field(j,weather,"n"),&total_n) || total_n>UINT32_MAX ||
       !token_uint(j,field(j,weather,"raw_n"),&total_raw) || total_raw>128 ||
       !token_uint(j,field(j,weather,"raw_dropped"),&total_dropped) || total_dropped>UINT32_MAX ||
       total_raw+total_dropped!=total_n) return 0;
    if(!token_text(j,field(j,record,"frame"),"ENU") ||
       !token_text(j,field(j,record,"time_basis"),"pixhawk_boot_us") ||
       !token_text(j,field(j,record,"host_time_basis"),"collector_monotonic_ms")) return 0;
    {
        unsigned origin=field(j,record,"origin_wgs84");
        if(!null_token(j,origin)) {
            unsigned item; double latitude,longitude;
            if(!vector3(j,origin)) return 0;
            item=origin+1;
            if(!token_double(j,item,&latitude) || latitude < -90 || latitude > 90) return 0;
            item=j->token[item].next;
            if(!token_double(j,item,&longitude) || longitude < -180 || longitude > 180) return 0;
        }
    }
    for(unsigned metric=0;metric<TS_METRIC_COUNT;++metric) {
        unsigned array=field(j,measurements,metric_fields[metric]);
        if(array>=j->count || j->token[array].kind!='[') return 0;
        for(unsigned g=array+1;g<j->token[array].next;g=j->token[g].next) {
            unsigned stats=field(j,g,"statistics"),samples=field(j,g,"samples"),raw_count=0;
            unsigned rep=field(j,g,"representative");
            uint64_t id,message_id,instance,n,raw,dropped,known,first=0,last=0;
            double mean,minimum,maximum,representative;
            if(groups>=16 || !token_uint(j,field(j,g,"message_id"),&message_id) || message_id>UINT32_C(0x00ffffff) ||
               !token_uint(j,field(j,g,"sensor_id"),&id) || id>UINT32_MAX ||
               !token_uint(j,field(j,g,"sensor_instance"),&instance) || instance>255 ||
               !weather_sample_record(j,rep,(int)metric) ||
               !token_uint(j,field(j,stats,"n"),&n) || !n || n>UINT32_MAX ||
               !token_uint(j,field(j,g,"raw_n"),&raw) || raw>128 ||
               !token_uint(j,field(j,g,"raw_dropped"),&dropped) || dropped>UINT32_MAX || raw+dropped!=n ||
               !token_uint(j,field(j,stats,"time_known_n"),&known) || known>n ||
               !token_double(j,field(j,stats,"mean"),&mean) ||
               !token_double(j,field(j,stats,"min"),&minimum) || minimum<0 ||
               !token_double(j,field(j,stats,"max"),&maximum) || maximum<minimum ||
               mean<minimum || mean>maximum || (metric && maximum>1) ||
               !token_double(j,field(j,rep,"value"),&representative) || representative<minimum || representative>maximum ||
               !nullable_nonnegative(j,field(j,stats,"variance")) ||
               (null_token(j,field(j,stats,"variance"))!=(n==1)) ||
               samples>=j->count || j->token[samples].kind!='[') return 0;
            if(known) {
                if(!token_u64_string(j,field(j,stats,"t_start_us"),&first) ||
                   !token_u64_string(j,field(j,stats,"t_end_us"),&last) || first>last) return 0;
            } else if(!null_token(j,field(j,stats,"t_start_us")) || !null_token(j,field(j,stats,"t_end_us"))) return 0;
            for(unsigned previous=0;previous<groups;++previous)
                if(ids[previous]==id && messages[previous]==message_id && instances[previous]==instance && metrics[previous]==metric) return 0;
            ids[groups]=id; messages[groups]=message_id; instances[groups]=(unsigned)instance; metrics[groups++]=metric;
            for(unsigned sample=samples+1;sample<j->token[samples].next;sample=j->token[sample].next) {
                uint64_t timestamp;
                if(++raw_count>128 || !weather_sample_record(j,sample,(int)metric)) return 0;
                if(token_u64_string(j,field(j,sample,"t_us"),&timestamp) &&
                   (!known || timestamp<first || timestamp>last)) return 0;
            }
            if(raw_count!=raw) return 0;
            sum_n+=n; sum_raw+=raw; sum_dropped+=dropped;
        }
    }
    return (groups || spatial_n) && sum_n==total_n && sum_raw==total_raw && sum_dropped==total_dropped;
}
static int validate_body(const char *body,size_t length,unsigned records[TS_RECORD_CAP],unsigned *count) {
    ts_json *j=&server.json;
    unsigned array,i; int weather,status;
    j->text=body; j->size=length; j->pos=0; j->count=0; *count=0;
    if(parse_value(j,0)==TS_NONE) return 0;
    whitespace(j);
    weather=token_text(j,field(j,0,"schema"),"mavlink_weather_v1");
    status=token_text(j,field(j,0,"schema"),"mavlink_router_status_v1");
    if(j->pos!=j->size || (!weather && !status && !token_text(j,field(j,0,"schema"),"mavlink_router_test_v1")) ||
       ((weather || status) && !nonempty_string(j,field(j,0,"stream")))) return 0;
    array=field(j,0,"records");
    if(array==TS_NONE || j->token[array].kind!='[') return 0;
    for(i=array+1;i<j->token[array].next;i=j->token[i].next) {
        if(*count>=TS_RECORD_CAP || !(status ? status_record(j,i) : weather ? weather_body_record(j,i) : validate_record(j,i))) return 0;
        records[(*count)++]=i;
    }
    return *count>0;
}
static void terminal_size(unsigned *rows,unsigned *columns) {
    *rows=24; *columns=80;
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info;
    if(GetConsoleScreenBufferInfo(GetStdHandle(STD_ERROR_HANDLE),&info)) {
        *rows=(unsigned)(info.srWindow.Bottom-info.srWindow.Top+1);
        *columns=(unsigned)(info.srWindow.Right-info.srWindow.Left+1);
    }
#else
    struct winsize size;
    if(!ioctl(STDERR_FILENO,TIOCGWINSZ,&size)) {
        if(size.ws_row) *rows=size.ws_row;
        if(size.ws_col) *columns=size.ws_col;
    }
#endif
    if(*rows>MF_MAX_ROWS) *rows=MF_MAX_ROWS;
}
typedef struct {
    mf_frame *frame;
    unsigned rows,columns,line;
} ts_writer;
/* Diagnostics contain formatted ASCII only. Keep colour outside the clipped
 * text and reserve the last terminal column, so a row can never auto-wrap. */
static void screen_line(ts_writer *writer,const char *colour,const char *format,...) {
    char text[512];
    size_t length;
    va_list args;
    if(writer->line>=writer->rows) return;
    va_start(args,format); (void)vsnprintf(text,sizeof(text),format,args); va_end(args);
    length=strlen(text);
    if(length>=writer->columns) length=writer->columns ? writer->columns-1u : 0;
    if(colour) (void)mf_append(writer->frame,colour,strlen(colour));
    (void)mf_append(writer->frame,text,length);
    if(colour) (void)mf_append(writer->frame,"\033[0m",4);
    (void)mf_append(writer->frame,"\n",1);
    ++writer->line;
}
/* Same row-diff presenter as the router. Frames stay in fixed storage; socket
 * waits refresh elapsed time and resize even when no records arrive. */
static struct {
    mf_frame frame[2];
    unsigned next,rows,columns;
    uint64_t next_ms;
    int valid;
} display;
static uint64_t receipt_age(uint64_t now,uint64_t received,uint64_t source_age) {
    uint64_t elapsed=now>=received ? now-received : 0;
    return UINT64_MAX-source_age<elapsed ? UINT64_MAX : source_age+elapsed;
}
static void age_text(int known,uint64_t age,char *text,size_t capacity) {
    if(known) (void)snprintf(text,capacity,"%.1fs",(double)age/1000.0);
    else (void)snprintf(text,capacity,"--");
}
static double display_value(double value,ts_metric_kind metric) {
    return metric==TS_METRIC_HUMIDITY ? value*100.0 : value-273.15;
}
static const char *metric_unit(ts_metric_kind metric) { return metric==TS_METRIC_HUMIDITY ? "%" : "C"; }
static const char *metric_name(ts_metric_kind metric) { return metric==TS_METRIC_HUMIDITY ? "RH" : "T"; }
static void metric_text(const ts_metric *metric,ts_metric_kind kind,uint64_t now,char *text,size_t capacity) {
    char age[32];
    age_text(metric->age_known,receipt_age(now,metric->received_ms,metric->source_age_ms),age,sizeof(age));
    if(metric->known) (void)snprintf(text,capacity,"%s %.2f%s [sensor_age=%s]",metric_name(kind),display_value(metric->value,kind),metric_unit(kind),age);
    else (void)snprintf(text,capacity,"%s --",metric_name(kind));
}
static int dashboard(int force) {
    unsigned rows,columns,visible,key_rows;
    uint64_t now;
    char last_age[32],status_age[32],temperature[96],humidity[96],position_age[32];
    ts_writer writer;
    const mf_frame *previous;
    int repaint;
    if(!server.ansi) return 1;
    now=now_ms();
    if(!force && now<display.next_ms) return !ferror(stderr);
    display.next_ms=now+TS_REFRESH_MS;
    terminal_size(&rows,&columns);
    writer.frame=&display.frame[display.next];
    writer.rows=rows; writer.columns=columns; writer.line=0;
    mf_reset(writer.frame);
    screen_line(&writer,"\033[32m","[test server] LISTENING");
    screen_line(&writer,NULL,"http://%s:%u/ingest  UDP=%u  run_elapsed=%.1fs  Ctrl+C stops",server.bind,server.port,server.udp_port,(double)(now-server.started)/1000.0);
    screen_line(&writer,NULL,"records=%" PRIu64 " statuses=%" PRIu64 " keys=%u%s IDs=%u rejected=%" PRIu64,
                server.records,server.statuses,server.key_n,server.keys_overflow ? "+" : "",server.id_n,server.rejected);
    age_text(server.order!=0,receipt_age(now,server.last_received_ms,0),last_age,sizeof(last_age));
    age_text(server.have_status,receipt_age(now,server.status_received_ms,0),status_age,sizeof(status_age));
    screen_line(&writer,NULL,"last_rx=%s | link=%s %u.%u (report_age=%s)",last_age,
                !server.have_status ? "--" : server.connected ? "UP" : "DOWN",server.link_sysid,server.link_compid,status_age);
    metric_text(&server.metrics[TS_METRIC_TEMPERATURE],TS_METRIC_TEMPERATURE,now,temperature,sizeof(temperature));
    metric_text(&server.metrics[TS_METRIC_HUMIDITY],TS_METRIC_HUMIDITY,now,humidity,sizeof(humidity));
    screen_line(&writer,NULL,"%s | %s",temperature,humidity);
    age_text(server.position.age_known,receipt_age(now,server.position.received_ms,server.position.source_age_ms),position_age,sizeof(position_age));
    if(server.position.known) {
        const char *state=!strcmp(server.position.state,"LIVE") ? "" : server.position.state;
        screen_line(&writer,*state ? "\033[33m" : NULL,"last %s %.6f %.6f %.1fm age=%s %s fix=%u",
                    server.position.source,server.position.latitude,server.position.longitude,server.position.altitude,
                    position_age,state,server.position.fix);
    } else screen_line(&writer,NULL,"GPS -- (no coordinate received)");
    {
        ub_report udp={0};
        ub_receiver_get_report(server.udp,&udp);
        if(server.udp_error) screen_line(&writer,"\033[33m","HTTP in=%" PRIu64 " ok=%" PRIu64 " | UDP ERROR=%u port=%u",
                    server.requests,server.accepted,server.udp_error,server.udp_port);
        else screen_line(&writer,NULL,"HTTP in=%" PRIu64 " ok=%" PRIu64 " | UDP in=%" PRIu64 " ok=%" PRIu64 " drop=%" PRIu64 " bad=%" PRIu64 " | dup=%" PRIu64,
                    server.requests,server.accepted,server.udp_messages,server.udp_accepted,
                    udp.drops,udp.errors+server.udp_rejected,server.cross_duplicates);
    }
    /* Short terminals keep the newest arrival visible. Large terminals show a
     * stable key table instead of pushing each JSON detail into a scrolling log. */
    key_rows=rows>13 ? rows-13 : 0;
    if(key_rows>TS_KEY_VISIBLE) key_rows=TS_KEY_VISIBLE;
    if(key_rows>server.key_n) key_rows=server.key_n;
    if(key_rows) {
        screen_line(&writer,NULL,"KEY: metric ID/msg/sensor/instance       VALUE       RX n  seq     last_rx");
        for(unsigned i=0;i<key_rows;++i) {
            const ts_key *key=&server.keys[i];
            char age[32];
            age_text(1,receipt_age(now,key->received_ms,0),age,sizeof(age));
            screen_line(&writer,NULL,"%-2s %u.%u/%u/%u/%u  %10.2f%-1s  %7" PRIu64 "  %-7" PRIu64 " %s",
                        metric_name(key->metric),key->sysid,key->compid,key->message,key->sensor,key->instance,
                        display_value(key->value,key->metric),metric_unit(key->metric),key->observations,key->seq,age);
        }
        if(key_rows<server.key_n) screen_line(&writer,NULL,"... %u more keys retained",server.key_n-key_rows);
    }
    if(rows>9) screen_line(&writer,NULL,"ARRIVALS newest first (deliveries; cross-transport duplicates skipped)");
    visible=rows>writer.line ? rows-writer.line : 0;
    if(visible>server.log_n) visible=server.log_n;
    if(!server.log_n) screen_line(&writer,NULL,"Waiting for data...");
    for(unsigned i=0;i<visible;++i)
        screen_line(&writer,NULL,"%s",server.log[(server.log_next+TS_LOG_CAP-1u-i)%TS_LOG_CAP].text);
    previous=display.valid ? &display.frame[display.next^1u] : NULL;
    repaint=display.rows!=rows || display.columns!=columns;
    display.valid=mf_present(stderr,writer.frame,previous,rows,columns,repaint);
    if(display.valid) {
        display.next^=1u; display.rows=rows; display.columns=columns;
    }
    return display.valid;
}
static void log_line(const char *format,...) {
    va_list args;
    ts_log *line=&server.log[server.log_next];
    va_start(args,format); (void)vsnprintf(line->text,sizeof(line->text),format,args); va_end(args);
    { size_t used=strlen(line->text);
      (void)snprintf(line->text+used,sizeof(line->text)-used," [%s]",server.transport==TS_UDP ? "UDP" : "HTTP"); }
    server.log_next=(server.log_next+1)%TS_LOG_CAP;
    if(server.log_n<TS_LOG_CAP) ++server.log_n;
}
static void status_text(const ts_json *j,unsigned token,char *text,size_t capacity) {
    size_t n=0;
    if(!capacity) return;
    if(token<j->count && j->token[token].kind=='"') {
        const ts_token *t=&j->token[token];
        for(size_t i=t->start+1;i<t->end-1 && n+1<capacity;++i) {
            unsigned char c=(unsigned char)j->text[i];
            text[n++]=(char)(c>=32 && c<127 ? c : '?');
        }
    }
    text[n]=0;
}
static void remember_id(unsigned sysid,unsigned compid) {
    unsigned id=(sysid<<8)|compid,byte=id/8u,bit=1u<<(id%8u);
    if(id>=TS_ID_BITS || (!sysid && !compid)) return;
    if(!(server.ids[byte]&bit)) { server.ids[byte]|=(unsigned char)bit; ++server.id_n; }
}
static void remember_position(const ts_json *j,unsigned object,uint64_t received,int status) {
    ts_position *position=&server.position;
    uint64_t age=0,fix=0;
    if(object>=j->count) return; /* Older status clients omit the optional field. */
    memset(position,0,sizeof(*position));
    if(null_token(j,object)) return;
    position->known=token_double(j,field(j,object,"lat_deg"),&position->latitude) &&
        token_double(j,field(j,object,"lon_deg"),&position->longitude) &&
        token_double(j,field(j,object,"alt_msl_m"),&position->altitude);
    position->received_ms=received;
    if(status) {
        position->age_known=token_u64_string(j,field(j,object,"rx_age_ms"),&age);
        (void)token_uint(j,field(j,object,"fix"),&fix);
        status_text(j,field(j,object,"source"),position->source,sizeof(position->source));
        status_text(j,field(j,object,"state"),position->state,sizeof(position->state));
    } else {
        (void)snprintf(position->source,sizeof(position->source),"GPS");
    }
    position->source_age_ms=age; position->fix=(unsigned)fix;
}
static void print_status(unsigned record) {
    ts_json *j=&server.json;
    unsigned source=field(j,record,"source"),weather=field(j,record,"weather");
    unsigned devices=field(j,record,"devices");
    uint64_t seq=0,sysid=0,compid=0,received=server.last_received_ms;
    (void)token_u64_string(j,field(j,record,"seq"),&seq);
    (void)token_uint(j,field(j,source,"sysid"),&sysid);
    (void)token_uint(j,field(j,source,"compid"),&compid);
    server.link_sysid=(unsigned)sysid; server.link_compid=(unsigned)compid;
    server.have_status=1; server.status_received_ms=received;
    server.connected=j->token[field(j,source,"connected")].kind=='t';
    status_text(j,field(j,source,"state"),server.link_state,sizeof(server.link_state));
    remember_id((unsigned)sysid,(unsigned)compid);
    for(unsigned d=devices+1;d<j->token[devices].next;d=j->token[d].next) {
        (void)token_uint(j,field(j,d,"sysid"),&sysid);
        (void)token_uint(j,field(j,d,"compid"),&compid);
        remember_id((unsigned)sysid,(unsigned)compid);
    }
    for(unsigned metric=0;metric<TS_METRIC_COUNT;++metric) {
        unsigned object=field(j,weather,metric_fields[metric]);
        ts_metric *value=&server.metrics[metric];
        value->known=token_double(j,field(j,object,"value"),&value->value);
        value->age_known=token_u64_string(j,field(j,object,"rx_age_ms"),&value->source_age_ms);
        value->received_ms=received;
    }
    remember_position(j,field(j,record,"position"),received,1);
    log_line("#%" PRIu64 " STATUS seq=%" PRIu64 " ID=%u.%u link=%s",server.order,seq,
             server.link_sysid,server.link_compid,server.connected ? "UP" : server.link_state);
}
static ts_key *remember_key(unsigned sysid,unsigned compid,unsigned message,unsigned sensor,
                            unsigned instance,ts_metric_kind metric) {
    unsigned index=server.key_n;
    for(unsigned i=0;i<server.key_n;++i) {
        const ts_key *key=&server.keys[i];
        if(key->sysid==sysid && key->compid==compid && key->message==message &&
           key->sensor==sensor && key->instance==instance && key->metric==metric) return &server.keys[i];
    }
    if(index==TS_KEY_CAP) {
        server.keys_overflow=1; index=0;
        for(unsigned i=1;i<server.key_n;++i)
            if(server.keys[i].received_ms<server.keys[index].received_ms) index=i;
    } else ++server.key_n;
    memset(&server.keys[index],0,sizeof(server.keys[index]));
    server.keys[index].sysid=sysid; server.keys[index].compid=compid;
    server.keys[index].message=message; server.keys[index].sensor=sensor;
    server.keys[index].instance=instance; server.keys[index].metric=metric;
    return &server.keys[index];
}
static void print_weather(unsigned record) {
    ts_json *j=&server.json;
    unsigned weather=field(j,record,"weather"),measurements=field(j,weather,"measurements"),source=field(j,record,"source");
    uint64_t seq=0,sysid=0,compid=0,total=0;
    (void)token_uint(j,field(j,record,"seq"),&seq);
    (void)token_uint(j,field(j,source,"sysid"),&sysid);
    (void)token_uint(j,field(j,source,"compid"),&compid);
    (void)token_uint(j,field(j,weather,"n"),&total);
    remember_id((unsigned)sysid,(unsigned)compid);
    for(unsigned metric=0;metric<TS_METRIC_COUNT;++metric) {
        unsigned array=field(j,measurements,metric_fields[metric]);
        for(unsigned group=array+1;group<j->token[array].next;group=j->token[group].next) {
            unsigned rep=field(j,group,"representative"),stats=field(j,group,"statistics");
            uint64_t id=0,message=0,instance=0,n=0;
            double value=0;
            ts_key *key;
            (void)token_uint(j,field(j,group,"sensor_id"),&id);
            (void)token_uint(j,field(j,group,"message_id"),&message);
            (void)token_uint(j,field(j,group,"sensor_instance"),&instance);
            (void)token_uint(j,field(j,stats,"n"),&n);
            (void)token_double(j,field(j,rep,"value"),&value);
            key=remember_key((unsigned)sysid,(unsigned)compid,(unsigned)message,(unsigned)id,
                             (unsigned)instance,(ts_metric_kind)metric);
            key->value=value; key->seq=seq; key->received_ms=server.last_received_ms;
            key->observations=UINT64_MAX-key->observations<n ? UINT64_MAX : key->observations+n;
            server.metrics[metric].value=value; server.metrics[metric].known=1;
            server.metrics[metric].received_ms=server.last_received_ms;
            /* The measurement body has acquisition time, not cross-host receive
             * age. Do not reset sensor age to zero just because HTTP retried. */
            server.metrics[metric].age_known=0;
        }
    }
    log_line("#%" PRIu64 " WEATHER seq=%" PRIu64 " ID=%" PRIu64 ".%" PRIu64 " n=%" PRIu64,
             server.order,seq,sysid,compid,total);
}
static int print_record(unsigned record) {
    ts_json *j=&server.json;
    unsigned heartbeat=field(j,record,"heartbeat"),position=field(j,record,"position");
    uint64_t seq=0,sysid=0,compid=0,age=0;
    int status=token_text(j,field(j,0,"schema"),"mavlink_router_status_v1");
    ++server.order;
    server.last_received_ms=now_ms();
    if(status) print_status(record);
    else if(token_text(j,field(j,0,"schema"),"mavlink_weather_v1")) print_weather(record);
    else {
        (void)token_uint(j,field(j,record,"seq"),&seq);
        (void)token_uint(j,field(j,heartbeat,"sysid"),&sysid);
        (void)token_uint(j,field(j,heartbeat,"compid"),&compid);
        remember_id((unsigned)sysid,(unsigned)compid);
        if(!null_token(j,position)) {
            remember_position(j,position,server.last_received_ms,0);
            server.position.age_known=token_uint(j,field(j,record,"coordinate_age_ms"),&age);
            server.position.source_age_ms=age;
        }
        log_line("#%" PRIu64 " HEARTBEAT seq=%" PRIu64 " ID=%" PRIu64 ".%" PRIu64,server.order,seq,sysid,compid);
    }
    if(!server.ansi) {
        int quoted=0,escape=0;
        /* Emit one compact JSON record per line. Literal ESC/control bytes are
         * invalid JSON; UTF-8 encoded C1 controls are escaped for terminal safety. */
        for(size_t i=j->token[record].start;i<j->token[record].end;++i) {
            unsigned char c=(unsigned char)j->text[i];
            if(!quoted && strchr(" \r\n\t",c)) continue;
            if(quoted && c==0xc2 && i+1<j->token[record].end &&
               (unsigned char)j->text[i+1]>=0x80 && (unsigned char)j->text[i+1]<=0x9f) {
                fprintf(stdout,"\\u00%02x",(unsigned char)j->text[++i]); escape=0; continue;
            }
            if(fputc(c,stdout)==EOF) return 0;
            if(escape) escape=0;
            else if(quoted && c=='\\') escape=1;
            else if(c=='"') quoted=!quoted;
        }
        if(fputc('\n',stdout)==EOF || fflush(stdout)) return 0;
    }
    if(status) ++server.statuses;
    else ++server.records;
    return 1;
}
static int recent_record(unsigned record,ts_transport transport,int remember) {
    const ts_json *j=&server.json;
    unsigned stream=field(j,0,"stream"),schema;
    size_t length=j->token[record].end-j->token[record].start,stream_length;
    const char *bytes=j->text+j->token[record].start,*stream_bytes;
    ts_recent *entry;
    if(stream>=j->count) return 0;
    stream_length=j->token[stream].end-j->token[stream].start;
    if(stream_length>TS_STREAM_CAP || length>TS_BODY_CAP) return 0;
    stream_bytes=j->text+j->token[stream].start;
    schema=token_text(j,field(j,0,"schema"),"mavlink_router_status_v1") ? 1u :
           token_text(j,field(j,0,"schema"),"mavlink_weather_v1") ? 2u : 3u;
    for(unsigned i=0;i<TS_RECENT_CAP;++i) {
        entry=&server.recent[i];
        if(entry->length==length && entry->stream_length==stream_length && entry->schema==schema &&
           !memcmp(entry->stream,stream_bytes,stream_length) && !memcmp(entry->record,bytes,length))
            return entry->transport!=transport;
    }
    if(!remember) return 0;
    entry=&server.recent[server.recent_next];
    server.recent_next=(server.recent_next+1u)%TS_RECENT_CAP;
    entry->length=length; entry->stream_length=stream_length; entry->schema=schema; entry->transport=transport;
    memcpy(entry->stream,stream_bytes,stream_length); memcpy(entry->record,bytes,length);
    return 0;
}
/* Both transports validate the entire envelope before displaying any record.
 * No callback can run inside this function: the UDP pump only runs while the
 * HTTP socket waits, so its parser never overwrites a half-processed request. */
static unsigned ingest_body(const char *body,size_t length,ts_transport transport) {
    unsigned records[TS_RECORD_CAP],count=0;
    if(length>TS_BODY_CAP || !validate_body(body,length,records,&count)) return 400;
    server.transport=transport;
    server.last_received_ms=now_ms();
    for(unsigned i=0;i<count;++i) {
        if(recent_record(records[i],transport,0)) { ++server.cross_duplicates; continue; }
        if(!print_record(records[i])) return 500;
        (void)recent_record(records[i],transport,1);
    }
    return dashboard(1) ? 0 : 500;
}
static void udp_message(void *context,const char *body,size_t length) {
    (void)context;
    ++server.udp_messages;
    if(ingest_body(body,length,TS_UDP)) ++server.udp_rejected;
    else ++server.udp_accepted;
}
static void serve_client(ts_socket client) {
    size_t used=0,header_end=0,body_size=0,scan=0;
    uint64_t deadline=now_ms()+5000;
    while(!stopped() && now_ms()<deadline) {
        int ready=socket_ready(client,0),n;
        if(ready<0) break;
        if(!ready) continue;
        n=(int)recv(client,server.request+used,(int)(sizeof(server.request)-1-used),0);
        if(n<0) { if(would_block()) continue; break; }
        if(!n) break;
        used+=(size_t)n; server.request[used]=0;
        if(!header_end) {
            int expect_continue=0;
            for(;scan+3<used;++scan) if(!memcmp(server.request+scan,"\r\n\r\n",4)) { header_end=scan+4; break; }
            if(!header_end && used>=TS_HEADER_CAP) { reply(client,431,deadline); ++server.rejected; return; }
            if(header_end) {
                unsigned error;
                if(header_end>TS_HEADER_CAP) { reply(client,431,deadline); ++server.rejected; return; }
                error=parse_headers(server.request,header_end,&body_size,&expect_continue);
                if(error) { reply(client,error,deadline); ++server.rejected; return; }
                if(expect_continue && used-header_end<body_size && !send_all(client,"HTTP/1.1 100 Continue\r\n\r\n",25,deadline)) break;
            }
        }
        if(header_end && used-header_end>=body_size) {
            unsigned error=used-header_end!=body_size ? 400 : ingest_body(server.request+header_end,body_size,TS_HTTP);
            if(error) { reply(client,error,deadline); ++server.rejected; return; }
            ++server.accepted;
            reply(client,204,deadline);
            return;
        }
        if(used==sizeof(server.request)-1) { reply(client,413,deadline); ++server.rejected; return; }
    }
    ++server.rejected;
    if(!stopped()) reply(client,400,deadline);
}
static int cli_number(const char *text,uint64_t *value) { return unsigned_number(text,strlen(text),value); }
int main(int argc,char **argv) {
    if(argc==2 && !strcmp(argv[1],"--version")) {
        puts(TB_PRODUCT " test-server " TB_VERSION); return 0;
    }
    ts_socket listener=TS_INVALID;
    struct sockaddr_in address;
    uint64_t duration=0;
    int plain=0,result=1,udp_explicit=0;
#ifdef _WIN32
    WSADATA wsa; DWORD old_mode=0; int mode_changed=0,cursor_saved=0;
    CONSOLE_CURSOR_INFO old_cursor;
#endif
    memset(&server,0,sizeof(server)); server.port=21000;
    (void)snprintf(server.bind,sizeof(server.bind),"127.0.0.1");
    for(int i=1;i<argc;++i) {
        uint64_t value;
        if(!strcmp(argv[i],"--plain")) { plain=1; continue; }
        if(!strcmp(argv[i],"--help")) {
            puts("mavlink_telemetry_data_router_test_server [--bind IPv4] [--port 21000]\n  [--udp-port N] [--plain] [--duration-ms N] [--max-requests N]\nHTTP + framed UDP diagnostic; UDP defaults to TCP port+1 (0 disables).\nRAM only; --plain emits one JSON record per line. Cross-transport duplicates use a bounded recent cache."); return 0;
        }
        if(i+1>=argc) { fprintf(stderr,"Missing value: %s\n",argv[i]); return 2; }
        if(!strcmp(argv[i],"--bind")) { if(strlen(argv[++i])>=sizeof(server.bind)) return 2; (void)snprintf(server.bind,sizeof(server.bind),"%s",argv[i]); }
        else if(!strcmp(argv[i],"--port")) { if(!cli_number(argv[++i],&value) || !value || value>65535) return 2; server.port=(unsigned)value; }
        else if(!strcmp(argv[i],"--udp-port")) { if(!cli_number(argv[++i],&value) || value>65535) return 2; server.udp_port=(unsigned)value; udp_explicit=1; }
        else if(!strcmp(argv[i],"--duration-ms")) { if(!cli_number(argv[++i],&duration) || duration>UINT32_MAX) return 2; }
        else if(!strcmp(argv[i],"--max-requests")) { if(!cli_number(argv[++i],&server.max_requests)) return 2; }
        else { fprintf(stderr,"Unknown option: %s\n",argv[i]); return 2; }
    }
    if(!udp_explicit) {
        if(server.port==65535) { fputs("TCP port 65535 needs explicit --udp-port N (0 disables)\n",stderr); return 2; }
        server.udp_port=server.port+1u;
    }
    memset(&address,0,sizeof(address)); address.sin_family=AF_INET; address.sin_port=htons((uint16_t)server.port);
#ifdef _WIN32
    if(WSAStartup(MAKEWORD(2,2),&wsa)) { fputs("Winsock initialization failed\n",stderr); return 1; }
#endif
    if(inet_pton(AF_INET,server.bind,&address.sin_addr)!=1) { fputs("--bind requires an IPv4 literal\n",stderr); goto done; }
    listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if(listener==TS_INVALID) goto done;
#ifdef _WIN32
    { BOOL exclusive=TRUE; if(setsockopt(listener,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,(const char *)&exclusive,sizeof(exclusive))) goto done; }
#else
    /* TCP TIME_WAIT must not prevent restarting this local test endpoint.
     * listen() still rejects another live listener on the same address. */
    { int reuse=1; if(setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse))) goto done; }
#endif
    if(bind(listener,(const struct sockaddr *)&address,sizeof(address)) || listen(listener,8) || !nonblock(listener)) {
        fputs("Could not listen: address/port unavailable\n",stderr); goto done;
    }
    if(server.udp_port) {
        unsigned error=0;
        server.udp=ub_receiver_open(server.bind,server.udp_port,udp_message,NULL,&error);
        if(!server.udp) {
            server.udp_error=error ? error : 1u;
            fprintf(stderr,"UDP ERROR %s:%u (error=%u); HTTP remains available\n",server.bind,server.udp_port,server.udp_error);
        }
    }
    if(!plain) {
#ifdef _WIN32
        HANDLE output=GetStdHandle(STD_ERROR_HANDLE);
        if(_isatty(_fileno(stderr)) && GetConsoleMode(output,&old_mode) && SetConsoleMode(output,old_mode|ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
            server.ansi=1; mode_changed=1;
            cursor_saved=GetConsoleCursorInfo(output,&old_cursor)!=0;
        }
#else
        server.ansi=(unsigned)(isatty(STDERR_FILENO)!=0);
#endif
    }
#ifdef _WIN32
    (void)SetConsoleCtrlHandler(stop_console,TRUE);
#else
    { struct sigaction action;
      memset(&action,0,sizeof(action)); action.sa_handler=stop_signal;
      (void)sigemptyset(&action.sa_mask); (void)sigaction(SIGINT,&action,NULL); (void)sigaction(SIGTERM,&action,NULL);
      action.sa_handler=SIG_IGN; (void)sigaction(SIGPIPE,&action,NULL); }
#endif
    server.started=now_ms(); server.end_ms=duration ? server.started+duration : 0;
    if(server.ansi) (void)dashboard(1);
    else fprintf(stderr,"LISTENING TCP HTTP http://%s:%u/ingest UDP=%u (memory-only, max_body=%u)\n",server.bind,server.port,server.udp_port,TS_BODY_CAP);
    fflush(stderr);
    while(!stopped() && (!server.max_requests || server.requests<server.max_requests)) {
        int ready=socket_ready(listener,0);
        ts_socket client;
        if(ready<0) break;
        if(!ready) continue;
        client=accept(listener,NULL,NULL);
        if(client==TS_INVALID) { if(would_block()) continue; break; }
        ++server.requests;
        if(nonblock(client)) serve_client(client); else ++server.rejected;
        (void)ts_close(client);
        (void)dashboard(0);
    }
    if(server.ansi) { (void)dashboard(1); fputs("\033[0m\033[?25h\n",stderr); }
    {
        ub_report udp={0};
        ub_receiver_get_report(server.udp,&udp);
        fprintf(stderr,"STOPPED requests=%" PRIu64 " accepted=%" PRIu64 " records=%" PRIu64 " rejected=%" PRIu64 " statuses=%" PRIu64
                " udp_messages=%" PRIu64 " udp_accepted=%" PRIu64 " udp_rejected=%" PRIu64 " udp_drop=%" PRIu64 " cross_duplicates=%" PRIu64 " udp_errors=%" PRIu64 " udp_bind_error=%u\n",
                server.requests,server.accepted,server.records,server.rejected,server.statuses,
                server.udp_messages,server.udp_accepted,server.udp_rejected,udp.drops,server.cross_duplicates,udp.errors,server.udp_error);
    }
    result=0;
done:
    ub_receiver_close(server.udp);
    if(listener!=TS_INVALID) (void)ts_close(listener);
#ifdef _WIN32
    if(cursor_saved) (void)SetConsoleCursorInfo(GetStdHandle(STD_ERROR_HANDLE),&old_cursor);
    if(mode_changed) (void)SetConsoleMode(GetStdHandle(STD_ERROR_HANDLE),old_mode);
    (void)SetConsoleCtrlHandler(stop_console,FALSE); WSACleanup();
#endif
    return result;
}
