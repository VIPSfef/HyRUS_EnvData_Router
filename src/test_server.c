/* Optional loopback diagnostic HTTP server. Fixed storage; no observation files.
 * Parsing is O(header bytes + JSON bytes), nesting <=32, tokens <=8192.
 * Each accepted connection has a 5 s total deadline and <=100 ms polls.
 * The single diagnostic connection is deliberately isolated from the router's
 * receive/HTTP worker threads. 204 means fully validated and printed, not disk
 * persistence. This executable is never part of the ordinary four app builds. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
#define TS_BODY_CAP (128u*1024u)
#define TS_TOKEN_CAP 8192u
#define TS_LOG_CAP 64u
#define TS_RECORD_CAP 64u
#define TS_NONE UINT32_MAX

typedef struct { uint32_t start,end,next; char kind; } ts_token;
typedef struct { const char *text; size_t size,pos; unsigned count; ts_token token[TS_TOKEN_CAP]; } ts_json;
typedef struct { char text[192]; } ts_log;
typedef struct {
    uint64_t started,end_ms,requests,accepted,records,rejected,max_requests;
    unsigned port,ansi,log_n,log_next;
    char bind[64];
    ts_log log[TS_LOG_CAP];
    char request[TS_HEADER_CAP+TS_BODY_CAP+1u];
    ts_json json;
} ts_server;
static ts_server server;
static volatile sig_atomic_t stop_requested;

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
    FD_ZERO(&set); FD_SET(socket_fd,&set);
    rc=select((int)(socket_fd+1),writing ? NULL : &set,writing ? &set : NULL,NULL,&tv);
    if(rc<0 && would_block()) return 0;
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
static int validate_body(const char *body,size_t length,unsigned records[TS_RECORD_CAP],unsigned *count) {
    ts_json *j=&server.json;
    unsigned array,i;
    j->text=body; j->size=length; j->pos=0; j->count=0; *count=0;
    if(parse_value(j,0)==TS_NONE) return 0;
    whitespace(j);
    if(j->pos!=j->size || !token_text(j,field(j,0,"schema"),"mavlink_router_test_v1")) return 0;
    array=field(j,0,"records");
    if(array==TS_NONE || j->token[array].kind!='[') return 0;
    for(i=array+1;i<j->token[array].next;i=j->token[i].next) {
        if(*count>=TS_RECORD_CAP || !validate_record(j,i)) return 0;
        records[(*count)++]=i;
    }
    return *count>0;
}
static unsigned terminal_rows(void) {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info;
    if(GetConsoleScreenBufferInfo(GetStdHandle(STD_ERROR_HANDLE),&info)) return (unsigned)(info.srWindow.Bottom-info.srWindow.Top+1);
#else
    struct winsize size;
    if(!ioctl(STDERR_FILENO,TIOCGWINSZ,&size) && size.ws_row) return size.ws_row;
#endif
    return 24;
}
static unsigned terminal_columns(void) {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info;
    if(GetConsoleScreenBufferInfo(GetStdHandle(STD_ERROR_HANDLE),&info)) return (unsigned)(info.srWindow.Right-info.srWindow.Left+1);
#else
    struct winsize size;
    if(!ioctl(STDERR_FILENO,TIOCGWINSZ,&size) && size.ws_col) return size.ws_col;
#endif
    return 80;
}
static void dashboard(void) {
    unsigned rows=terminal_rows(),columns=terminal_columns(),visible=rows>7 ? rows-7 : 1;
    unsigned start;
    if(!server.ansi) return;
    if(visible>server.log_n) visible=server.log_n;
    if(visible>TS_LOG_CAP) visible=TS_LOG_CAP;
    start=(server.log_next+TS_LOG_CAP-visible)%TS_LOG_CAP;
    fputs("\033[?25l\033[H\033[2J[test server] \033[32mLISTENING\033[0m\n",stderr);
    fprintf(stderr,"TCP HTTP http://%s:%u/ingest  uptime=%.1fs\n",server.bind,server.port,(double)(now_ms()-server.started)/1000.0);
    fprintf(stderr,"requests=%" PRIu64 " accepted=%" PRIu64 " records=%" PRIu64 " rejected=%" PRIu64 "\n",server.requests,server.accepted,server.records,server.rejected);
    fputs("HTTP 204 = complete validated records displayed; memory-only diagnostic.\n",stderr);
    fputs("Ctrl+C stops. Recent heartbeat and position records:\n",stderr);
    if(!visible) fputs("Waiting for the router test stream...\n",stderr);
    for(unsigned i=0;i<visible;++i) fprintf(stderr,"%.*s\n",(int)(columns ? columns-1 : 79),server.log[(start+i)%TS_LOG_CAP].text);
    fflush(stderr);
}
static int print_record(unsigned record) {
    ts_json *j=&server.json;
    unsigned heartbeat=field(j,record,"heartbeat"),position=field(j,record,"position");
    uint64_t seq=0,sysid=0,compid=0,type=0,autopilot=0,age=0;
    ts_log *line=&server.log[server.log_next];
    (void)token_uint(j,field(j,record,"seq"),&seq);
    (void)token_uint(j,field(j,heartbeat,"sysid"),&sysid);
    (void)token_uint(j,field(j,heartbeat,"compid"),&compid);
    (void)token_uint(j,field(j,heartbeat,"type"),&type);
    (void)token_uint(j,field(j,heartbeat,"autopilot"),&autopilot);
    if(j->token[position].kind=='n')
        (void)snprintf(line->text,sizeof(line->text),"#%" PRIu64 " heartbeat %" PRIu64 ".%" PRIu64 " type=%" PRIu64 " autopilot=%" PRIu64 " position=unavailable",seq,sysid,compid,type,autopilot);
    else {
        double lat=0,lon=0,alt=0;
        int have_age=token_uint(j,field(j,record,"coordinate_age_ms"),&age);
        (void)token_double(j,field(j,position,"lat_deg"),&lat);
        (void)token_double(j,field(j,position,"lon_deg"),&lon);
        (void)token_double(j,field(j,position,"alt_msl_m"),&alt);
        (void)snprintf(line->text,sizeof(line->text),"#%" PRIu64 " HB %" PRIu64 ".%" PRIu64 " lat=%.7f lon=%.7f alt=%.2fm age=%s%" PRIu64 "ms",seq,sysid,compid,lat,lon,alt,have_age ? "" : "?",age);
    }
    server.log_next=(server.log_next+1)%TS_LOG_CAP;
    if(server.log_n<TS_LOG_CAP) ++server.log_n;
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
    ++server.records;
    return 1;
}
static void serve_client(ts_socket client) {
    size_t used=0,header_end=0,body_size=0,scan=0;
    uint64_t deadline=now_ms()+5000;
    unsigned records[TS_RECORD_CAP],count=0;
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
            if(used-header_end!=body_size || !validate_body(server.request+header_end,body_size,records,&count)) {
                reply(client,400,deadline); ++server.rejected; return;
            }
            for(unsigned i=0;i<count;++i) if(!print_record(records[i])) { reply(client,500,deadline); ++server.rejected; return; }
            if(server.ansi) { dashboard(); if(ferror(stderr)) { reply(client,500,deadline); ++server.rejected; return; } }
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
    ts_socket listener=TS_INVALID;
    struct sockaddr_in address;
    uint64_t duration=0;
    int plain=0,result=1;
#ifdef _WIN32
    WSADATA wsa; DWORD old_mode=0; int mode_changed=0;
#endif
    memset(&server,0,sizeof(server)); server.port=21000;
    (void)snprintf(server.bind,sizeof(server.bind),"127.0.0.1");
    for(int i=1;i<argc;++i) {
        uint64_t value;
        if(!strcmp(argv[i],"--plain")) { plain=1; continue; }
        if(!strcmp(argv[i],"--help")) {
            puts("mavlink_telemetry_data_router_test_server [--bind IPv4] [--port 21000]\n  [--plain] [--duration-ms N] [--max-requests N]\nTCP HTTP diagnostic only. RAM ring log; no files. --plain emits one JSON record per line."); return 0;
        }
        if(i+1>=argc) { fprintf(stderr,"Missing value: %s\n",argv[i]); return 2; }
        if(!strcmp(argv[i],"--bind")) { if(strlen(argv[++i])>=sizeof(server.bind)) return 2; (void)snprintf(server.bind,sizeof(server.bind),"%s",argv[i]); }
        else if(!strcmp(argv[i],"--port")) { if(!cli_number(argv[++i],&value) || !value || value>65535) return 2; server.port=(unsigned)value; }
        else if(!strcmp(argv[i],"--duration-ms")) { if(!cli_number(argv[++i],&duration) || duration>UINT32_MAX) return 2; }
        else if(!strcmp(argv[i],"--max-requests")) { if(!cli_number(argv[++i],&server.max_requests)) return 2; }
        else { fprintf(stderr,"Unknown option: %s\n",argv[i]); return 2; }
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
    if(!plain) {
#ifdef _WIN32
        HANDLE output=GetStdHandle(STD_ERROR_HANDLE);
        if(_isatty(_fileno(stderr)) && GetConsoleMode(output,&old_mode) && SetConsoleMode(output,old_mode|ENABLE_VIRTUAL_TERMINAL_PROCESSING)) { server.ansi=1; mode_changed=1; }
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
    if(server.ansi) dashboard();
    else fprintf(stderr,"LISTENING TCP HTTP http://%s:%u/ingest (memory-only, max_body=%u)\n",server.bind,server.port,TS_BODY_CAP);
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
        if(server.ansi) dashboard();
    }
    if(server.ansi) fputs("\033[0m\033[?25h\n",stderr);
    fprintf(stderr,"STOPPED requests=%" PRIu64 " accepted=%" PRIu64 " records=%" PRIu64 " rejected=%" PRIu64 "\n",server.requests,server.accepted,server.records,server.rejected);
    result=0;
done:
    if(listener!=TS_INVALID) (void)ts_close(listener);
#ifdef _WIN32
    if(mode_changed) (void)SetConsoleMode(GetStdHandle(STD_ERROR_HANDLE),old_mode);
    (void)SetConsoleCtrlHandler(stop_console,FALSE); WSACleanup();
#endif
    return result;
}
