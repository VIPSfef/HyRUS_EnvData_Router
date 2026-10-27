#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "config.h"
#include "serial_baud.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define CFG_PATH_BYTES 4096
#define CFG_LINE_BYTES 4096
enum { URL, TOKEN, ENV_NAME, INPUT_MODE, SERIAL_PORT, WEBSOCKET_URL, UNIT_MAP, IPV4_LITERAL, HOST_ADDRESS, SECRET,
       REAL, UINT_VALUE, CFG_BOOL, BAUD_VALUE, OPTIONAL_INTERVAL };
typedef struct {
    const char *name;
    unsigned kind;
    size_t offset, size;
    double low, high;
} cfg_field;
#define STR_FIELD(name, kind, member) {name, kind, offsetof(tb_config, member), sizeof(((tb_config *)0)->member), 0, 0}
#define NUM_FIELD(name, kind, member, low, high) {name, kind, offsetof(tb_config, member), 0, low, high}
static const cfg_field fields[] = {
    STR_FIELD("url", URL, url),
    STR_FIELD("stream", TOKEN, stream),
    STR_FIELD("session", TOKEN, session),
    STR_FIELD("token_env", ENV_NAME, token_env),
    {"origin_lat", REAL, offsetof(tb_config, origin), 0, -90, 90},
    {"origin_lon", REAL, offsetof(tb_config, origin) + sizeof(double), 0, -180, 180},
    {"origin_alt", REAL, offsetof(tb_config, origin) + 2 * sizeof(double), 0, -1e7, 1e7},
    NUM_FIELD("send_interval_ms", UINT_VALUE, send_interval_ms, 1000, 60000),
    NUM_FIELD("connect_timeout_ms", UINT_VALUE, connect_timeout_ms, 100, 60000),
    NUM_FIELD("io_timeout_ms", UINT_VALUE, io_timeout_ms, 100, 60000),
    NUM_FIELD("max_windows", UINT_VALUE, max_windows, 1, UINT_MAX),
    NUM_FIELD("refresh_ms", UINT_VALUE, refresh_ms, 250, 10000),
    NUM_FIELD("idle_warn_ms", UINT_VALUE, idle_warn_ms, 250, 3600000),
    NUM_FIELD("ack_warn_ms", UINT_VALUE, ack_warn_ms, 250, 3600000),
    NUM_FIELD("summary", CFG_BOOL, summary, 0, 1),
    STR_FIELD("input_mode", INPUT_MODE, input_mode),
    STR_FIELD("serial_port", SERIAL_PORT, serial_port),
    NUM_FIELD("serial_baud", BAUD_VALUE, serial_baud, 9600, 921600),
    STR_FIELD("websocket_url", WEBSOCKET_URL, websocket_url),
    NUM_FIELD("auto_scan", CFG_BOOL, auto_scan, 0, 1),
    NUM_FIELD("probe_ms", UINT_VALUE, probe_ms, 100, 60000),
    NUM_FIELD("link_timeout_ms", UINT_VALUE, link_timeout_ms, 100, 3600000),
    NUM_FIELD("reconnect_ms", UINT_VALUE, reconnect_ms, 100, 60000),
    NUM_FIELD("target_system", UINT_VALUE, target_system, 0, 255),
    NUM_FIELD("target_component", UINT_VALUE, target_component, 0, 255),
    NUM_FIELD("system_time_uncert_us", UINT_VALUE, system_time_uncert_us, 0, UINT32_MAX),
    NUM_FIELD("accel_bound_mps2", REAL, accel_bound_mps2, 0, 1e6),
    NUM_FIELD("duration_ms", UINT_VALUE, duration_ms, 0, UINT32_MAX),
    STR_FIELD("unit_map", UNIT_MAP, unit_map),
    NUM_FIELD("origin_auto", CFG_BOOL, origin_auto, 0, 1),
    STR_FIELD("udp_bind", IPV4_LITERAL, udp_bind),
    NUM_FIELD("udp_port", UINT_VALUE, udp_port, 1, 65535),
    NUM_FIELD("udp_enabled", CFG_BOOL, udp_enabled, 0, 1),
    NUM_FIELD("spatial_step_m", REAL, spatial_step_m, 0.01, 10000),
    NUM_FIELD("spatial_gap_ms", UINT_VALUE, spatial_gap_ms, 100, 60000),
    NUM_FIELD("spatial_enabled", CFG_BOOL, spatial_enabled, 0, 1),
    NUM_FIELD("spatial_adaptive", CFG_BOOL, spatial_adaptive, 0, 1),
    NUM_FIELD("qgc_enabled", CFG_BOOL, qgc_enabled, 0, 1),
    STR_FIELD("qgc_host", IPV4_LITERAL, qgc_host),
    NUM_FIELD("qgc_port", UINT_VALUE, qgc_port, 1, 65535),
    NUM_FIELD("status_interval_ms", OPTIONAL_INTERVAL, status_interval_ms, 1000, 60000),
    NUM_FIELD("mavlink_rates_enabled", UINT_VALUE, mavlink_rates_enabled, 0, 1),
    NUM_FIELD("udp_backup_enabled", UINT_VALUE, udp_backup_enabled, 0, 1),
    STR_FIELD("udp_backup_host", HOST_ADDRESS, udp_backup_host),
    NUM_FIELD("udp_backup_port", UINT_VALUE, udp_backup_port, 0, 65535),
    NUM_FIELD("tcp_buffer_max_mb", UINT_VALUE, tcp_buffer_max_mb, 1,
              SIZE_MAX / CFG_MIB_BYTES < CFG_TCP_BUFFER_MAX_MB ?
               SIZE_MAX / CFG_MIB_BYTES : CFG_TCP_BUFFER_MAX_MB),
    STR_FIELD("token", SECRET, token),
    NUM_FIELD("http_min_interval_ms", UINT_VALUE, http_min_interval_ms,
              CFG_HTTP_MIN_INTERVAL_MS, 60000)
};
#define FIELD_COUNT (sizeof(fields) / sizeof(fields[0]))
/* cfg_load tracks duplicate fields in one fixed-width mask. */
typedef char cfg_fields_fit_seen_mask[(FIELD_COUNT <= 64) ? 1 : -1];

unsigned cfg_field_count(void) { return (unsigned)FIELD_COUNT; }
const char *cfg_field_name(unsigned index)
{ return index<FIELD_COUNT ? fields[index].name : NULL; }
int cfg_field_is_url(unsigned index)
{ return index<FIELD_COUNT && (fields[index].kind==URL || fields[index].kind==WEBSOCKET_URL); }
int cfg_field_is_secret(unsigned index)
{ return index<FIELD_COUNT && fields[index].kind==SECRET; }
const char *cfg_auth_token(const tb_config *c)
{
    if(!c) return NULL;
    return c->token[0] ? c->token : (c->token_env[0] ? getenv(c->token_env) : NULL);
}
int cfg_field_value(const tb_config *c,unsigned index,char *out,size_t cap,int reveal_url)
{
    const cfg_field *field;
    const char *value;
    int length;
    if(out && cap) out[0]='\0';
    if(!c || index>=FIELD_COUNT || !out || !cap) return 0;
    field=&fields[index];
    value=(const char *)c+field->offset;
    if(cfg_field_is_secret(index) && !reveal_url) {
        length=snprintf(out,cap,"%s",value[0] ? "********" : "");
    } else if(cfg_field_is_url(index) && !reveal_url) {
        char safe[CFG_LINE_BYTES];
        cfg_safe_url(value,safe,sizeof(safe));
        length=snprintf(out,cap,"%s",safe);
    } else if(field->kind<REAL) length=snprintf(out,cap,"%s",value);
    else if(field->kind==REAL) length=snprintf(out,cap,"%.17g",*(const double *)value);
    else if(field->kind==CFG_BOOL) length=snprintf(out,cap,"%d",*(const int *)value);
    else length=snprintf(out,cap,"%u",*(const unsigned *)value);
    return length>=0 && (size_t)length<cap;
}

static int fail(char *err, size_t cap, const char *message)
{
    if (err && cap) (void)snprintf(err, cap, "%s", message);
    return 0;
}
static void clear_error(char *err, size_t cap)
{
    if (err && cap) err[0] = '\0';
}
static int letter(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static int digit(unsigned char c) { return c >= '0' && c <= '9'; }
static int space(unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static int secret_valid(const char *s)
{
    const unsigned char *p=(const unsigned char *)s;
    while(*p) { if(*p<33 || *p>126) return 0; ++p; }
    return (size_t)(p-(const unsigned char *)s)<=CFG_AUTH_TOKEN_MAX;
}

static int token_valid(const char *s, int env_name)
{
    size_t i, n = strlen(s);
    if (!n || n > 64 || (env_name && !letter((unsigned char)s[0]) && s[0] != '_')) return 0;
    for (i = 0; i < n; ++i)
        if (!letter((unsigned char)s[i]) && !digit((unsigned char)s[i]) &&
            s[i] != '_' && (env_name || (s[i] != '-' && s[i] != '.'))) return 0;
    return 1;
}

static int port_value(const char *p, const char *end,unsigned *value)
{
    unsigned port = 0;
    if (p == end) return 0;
    while (p < end) {
        if (!digit((unsigned char)*p) || port > 6553) return 0;
        port = port * 10 + (unsigned)(*p++ - '0');
        if (port > 65535) return 0;
    }
    if(!port) return 0;
    *value=port;
    return 1;
}

/* The same bounded host parser serves HTTP authorities and UDP overrides.
 * Validation never performs DNS or accepts numeric IPv4 shorthand/octal. */
static int ipv4_range_valid(const char *text,const char *end)
{
    const char *p=text;
    unsigned part;
    for (part = 0; part < 4; ++part) {
        unsigned octet = 0, digits = 0;
        if (p==end || !digit((unsigned char)*p) || (*p == '0' && p+1<end && digit((unsigned char)p[1]))) return 0;
        while (p<end && digit((unsigned char)*p)) {
            if (++digits > 3) return 0;
            octet = octet * 10 + (unsigned)(*p++ - '0');
        }
        if (octet > 255) return 0;
        if (part == 3) return p==end;
        if (p==end || *p++ != '.') return 0;
    }
    return 0;
}
static int ipv4_literal_valid(const char *address)
{ return ipv4_range_valid(address,address+strlen(address)); }
static int hex_value(unsigned char c)
{
    if(c>='0' && c<='9') return c-'0';
    if(c>='a' && c<='f') return c-'a'+10;
    if(c>='A' && c<='F') return c-'A'+10;
    return -1;
}
static int ipv6_range_valid(const char *p,const char *end)
{
    unsigned groups=0;
    int compressed=0;
    if(p==end) return 0;
    if(*p==':') {
        if(p+1==end || p[1]!=':') return 0;
        compressed=1; p+=2;
        if(p==end) return 1;
    }
    while(p<end) {
        const char *piece=p;
        unsigned digits=0;
        while(p<end && hex_value((unsigned char)*p)>=0) { ++digits; ++p; }
        if(p<end && *p=='.') {
            if(groups>6 || !ipv4_range_valid(piece,end)) return 0;
            groups+=2; p=end; break;
        }
        if(!digits || digits>4 || ++groups>8) return 0;
        if(p==end) break;
        if(*p++!=':' || p==end) return 0;
        if(*p==':') {
            if(compressed) return 0;
            compressed=1; ++p;
            if(p==end) break;
        }
    }
    return compressed ? groups<8 : groups==8;
}
static int hostname_range_valid(const char *p,const char *end)
{
    const char *first=p,*label=p;
    int numeric=1;
    if(p==end || (size_t)(end-p)>253) return 0;
    for(;p<end;++p) {
        unsigned char ch=(unsigned char)*p;
        if(ch=='.') {
            if(p==label || p-label>63 || label[0]=='-' || p[-1]=='-') return 0;
            label=p+1;
        } else {
            if(!letter(ch) && !digit(ch) && ch!='-' && ch!='_') return 0;
            if(!digit(ch)) numeric=0;
        }
    }
    if(numeric) return ipv4_range_valid(first,end);
    return label==end || (end-label<=63 && label[0]!='-' && end[-1]!='-');
}
/* A bracketed literal is accepted in settings for convenience; socket APIs
 * receive only the unbracketed range. IPv6 scope IDs are intentionally absent. */
static int host_range(const char *text,const char *end,const char **host,size_t *length)
{
    const char *first=text,*last=end;
    int valid;
    if(first==last) return 0;
    if(*first=='[') {
        if(last[-1]!=']') return 0;
        ++first; --last;
        valid=ipv6_range_valid(first,last);
    } else valid=memchr(first,':',(size_t)(last-first)) ?
        ipv6_range_valid(first,last) : hostname_range_valid(first,last);
    if(!valid) return 0;
    if(host) *host=first;
    if(length) *length=(size_t)(last-first);
    return 1;
}
typedef struct { const char *host; size_t length; unsigned port; } cfg_url_parts;
static int url_parts(const char *url,int websocket,cfg_url_parts *parts)
{
    const char *host,*end,*p,*port=NULL;
    cfg_url_parts result={NULL,0,0};
    size_t i;
    if(websocket) {
        if(!*url) { if(parts) *parts=result; return 1; }
        if(!strncmp(url,"ws://",5)) { host=url+5; result.port=80; }
        else if(!strncmp(url,"wss://",6)) { host=url+6; result.port=443; }
        else return 0;
    } else {
        if(!strcmp(url,"-")) { if(parts) *parts=result; return 1; }
        if(!strncmp(url,"http://",7)) { host=url+7; result.port=80; }
        else if(!strncmp(url,"https://",8)) { host=url+8; result.port=443; }
        else return 0;
    }
    for(i=0;url[i];++i)
        if((unsigned char)url[i]<=32 || (unsigned char)url[i]==127 || url[i]=='\\' || url[i]=='#') return 0;
    end=host+strcspn(host,"/?");
    if(host==end) return 0;
    if(*host=='[') {
        p=memchr(host,']',(size_t)(end-host));
        if(!p || !host_range(host,p+1,&result.host,&result.length)) return 0;
        if(p+1!=end) { if(p[1]!=':') return 0; port=p+2; }
    } else {
        p=memchr(host,':',(size_t)(end-host));
        if(!hostname_range_valid(host,p ? p : end)) return 0;
        result.host=host; result.length=(size_t)((p ? p : end)-host);
        if(p) port=p+1;
    }
    if(port && !port_value(port,end,&result.port)) return 0;
    if(parts) *parts=result;
    return 1;
}
static int url_valid(const char *url,int websocket)
{ return url_parts(url,websocket,NULL); }

int cfg_udp_backup_destination(const tb_config *c,char *host,size_t host_cap,
                               unsigned *port,char *err,size_t err_cap)
{
    cfg_url_parts parsed;
    const char *selected;
    size_t length;
    unsigned destination_port;
    if(host && host_cap) host[0]='\0';
    if(port) *port=0;
    clear_error(err,err_cap);
    if(!c || !host || !host_cap || !port ||
       !memchr(c->url,'\0',sizeof(c->url)) ||
       !memchr(c->udp_backup_host,'\0',sizeof(c->udp_backup_host)))
        return fail(err,err_cap,"Invalid UDP backup destination arguments");
    if(!url_parts(c->url,0,&parsed)) return fail(err,err_cap,"Invalid HTTP URL for UDP backup");
    if(!strcmp(c->udp_backup_host,"auto")) {
        if(!parsed.host) return fail(err,err_cap,"UDP backup auto host requires an HTTP URL");
        selected=parsed.host; length=parsed.length;
    } else if(!host_range(c->udp_backup_host,c->udp_backup_host+strlen(c->udp_backup_host),&selected,&length))
        return fail(err,err_cap,"Invalid UDP backup host");
    destination_port=c->udp_backup_port;
    if(!destination_port) {
        if(!parsed.host) return fail(err,err_cap,"UDP backup auto port requires an HTTP URL");
        if(parsed.port==65535) return fail(err,err_cap,"UDP backup automatic port exceeds 65535; set an explicit port");
        destination_port=parsed.port+1;
    }
    if(destination_port>65535) return fail(err,err_cap,"Invalid UDP backup port");
    if(length>=host_cap) return fail(err,err_cap,"UDP backup host output is too small");
    memcpy(host,selected,length); host[length]='\0'; *port=destination_port;
    return 1;
}
static int ascii_path_valid(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    if (!*p) return 0;
    while (*p) { if (*p < 32 || *p == 127) return 0; ++p; }
    return 1;
}
static int map_key_valid(const char *s, size_t n)
{
    size_t i;
    if (!n || n > 31 || (!letter((unsigned char)s[0]) && s[0] != '_')) return 0;
    for (i = 1; i < n; ++i)
        if (!letter((unsigned char)s[i]) && !digit((unsigned char)s[i]) &&
            s[i] != '_' && s[i] != '.' && s[i] != '-') return 0;
    return 1;
}
static int map_unit_valid(const char *s, size_t n)
{
    size_t i;
    if (!n || n > 15) return 0;
    for (i = 0; i < n; ++i)
        if (!letter((unsigned char)s[i]) && !digit((unsigned char)s[i]) &&
            !strchr("_./%*^+-", s[i])) return 0;
    return 1;
}
static int unit_map_valid(const char *map)
{
    const char *p = map;
    while (*p) {
        const char *end = p + strcspn(p, ";");
        const char *colon = memchr(p, ':', (size_t)(end - p));
        const char *previous;
        size_t key_n;
        if (!colon) return 0;
        key_n = (size_t)(colon - p);
        if (!map_key_valid(p, key_n) ||
            !map_unit_valid(colon + 1, (size_t)(end - colon - 1))) return 0;
        for (previous = map; previous < p;) {
            const char *old_end = previous + strcspn(previous, ";");
            const char *old_colon = memchr(previous, ':', (size_t)(old_end - previous));
            if (old_colon && (size_t)(old_colon - previous) == key_n &&
                !memcmp(previous, p, key_n)) return 0;
            previous = *old_end ? old_end + 1 : old_end;
        }
        p = *end ? end + 1 : end;
    }
    return 1;
}
int cfg_unit_for_key(const tb_config *c, const char *key, char *out, size_t cap)
{
    const char *p;
    size_t key_n;
    if (!c || !key || !out || !cap || !memchr(c->unit_map, '\0', sizeof(c->unit_map))) return 0;
    key_n = strlen(key);
    for (p = c->unit_map; *p;) {
        const char *end = p + strcspn(p, ";");
        const char *colon = memchr(p, ':', (size_t)(end - p));
        if (colon && (size_t)(colon - p) == key_n && !memcmp(p, key, key_n)) {
            size_t n = (size_t)(end - colon - 1);
            if (!n || n >= cap) return 0;
            memcpy(out, colon + 1, n); out[n] = '\0';
            return 1;
        }
        p = *end ? end + 1 : end;
    }
    return 0;
}

static int field_index(const char *key)
{
    size_t i;
    for (i = 0; i < FIELD_COUNT; ++i)
        if (!strcmp(key, fields[i].name)) return (int)i;
    return -1;
}

static int field_valid(const tb_config *c, const cfg_field *f)
{
    const char *p = (const char *)c + f->offset;
    if (f->kind < REAL) {
        if (!memchr(p, '\0', f->size)) return 0;
        if (f->kind == URL) return url_valid(p, 0);
        if (f->kind == WEBSOCKET_URL) return url_valid(p, 1);
        if (f->kind == INPUT_MODE) return !strcmp(p, "mavlink") || !strcmp(p, "csv");
        if (f->kind == SERIAL_PORT) return ascii_path_valid(p);
        if (f->kind == UNIT_MAP) return unit_map_valid(p);
        if (f->kind == IPV4_LITERAL) return ipv4_literal_valid(p);
        if (f->kind == HOST_ADDRESS) return host_range(p,p+strlen(p),NULL,NULL);
        if (f->kind == SECRET) return secret_valid(p);
        return token_valid(p, f->kind == ENV_NAME);
    }
    if (f->kind == REAL) {
        double x = *(const double *)p;
        return isfinite(x) && x >= f->low && x <= f->high;
    }
    if (f->kind == CFG_BOOL) return *(const int *)p == 0 || *(const int *)p == 1;
    if (f->kind == BAUD_VALUE) return serial_baud_valid(*(const unsigned *)p);
    if (f->kind == OPTIONAL_INTERVAL && !*(const unsigned *)p) return 1;
    return *(const unsigned *)p >= f->low && *(const unsigned *)p <= f->high;
}

void cfg_defaults(tb_config *c)
{
    if (!c) return;
    memset(c, 0, sizeof(*c));
    strcpy(c->token_env, "TB_TOKEN");
    c->send_interval_ms = c->refresh_ms = 1000;
    c->http_min_interval_ms = CFG_HTTP_MIN_INTERVAL_MS;
    c->connect_timeout_ms = c->io_timeout_ms = 1000;
    c->max_windows = 4;
    c->idle_warn_ms = 2000;
    c->ack_warn_ms = 5000;
    strcpy(c->input_mode, "mavlink");
    strcpy(c->serial_port, "auto");
    c->serial_baud = SERIAL_BAUD_DEFAULT;
    c->auto_scan = 1;
    c->origin_auto = 1;
    c->probe_ms = 2000;
    c->link_timeout_ms = 5000;
    c->reconnect_ms = 1000;
    c->system_time_uncert_us = UINT32_MAX;
    c->accel_bound_mps2 = 10;
    strcpy(c->udp_bind, "127.0.0.1");
    c->udp_port = 14540;
    c->udp_enabled = 1;
    c->spatial_step_m = 10.0;
    c->spatial_gap_ms = 2000;
    c->spatial_enabled = 1;
    c->spatial_adaptive = 1;
    c->qgc_enabled = 1;
    strcpy(c->qgc_host, "127.0.0.1");
    c->qgc_port = 14550;
    strcpy(c->udp_backup_host,"auto");
    c->tcp_buffer_max_mb = CFG_TCP_BUFFER_DEFAULT_MB;
}

int cfg_set(tb_config *c, const char *key, const char *value, char *err, size_t cap)
{
    tb_config next;
    const cfg_field *f;
    const char *last;
    char text[2048], *target, *end;
    size_t len;
    int index;
    clear_error(err, cap);
    if (!c || !key || !value) return fail(err, cap, "Invalid configuration arguments");
    index = field_index(key);
    if (index < 0) return fail(err, cap, "Unknown configuration key");
    f = &fields[index];
    if(f->kind != SECRET) while (space((unsigned char)*value)) ++value;
    last = value + strlen(value);
    if(f->kind != SECRET) while (last > value && space((unsigned char)last[-1])) --last;
    len = (size_t)(last - value);
    if (len >= sizeof(text)) return fail(err, cap, "Configuration value is too long");
    memcpy(text, value, len); text[len] = '\0';
    next = *c;
    target = (char *)&next + f->offset;
    if (f->kind < REAL) {
        if (len >= f->size) return fail(err, cap, "Configuration text is too long");
        memcpy(target, text, len + 1);
    } else if (f->kind == REAL) {
        double x;
        errno = 0;
        x = strtod(text, &end);
        if (!len || errno || *end || !isfinite(x))
            return fail(err, cap, "Expected a finite numeric configuration value");
        *(double *)target = x;
    } else if (f->kind == CFG_BOOL) {
        if (!strcmp(text, "1")) *(int *)target = 1;
        else if (!strcmp(text, "0")) *(int *)target = 0;
        else return fail(err, cap, "Expected boolean 0 or 1");
    } else {
        unsigned long x;
        size_t i;
        for (i = 0; i < len; ++i)
            if (!digit((unsigned char)text[i])) return fail(err, cap, "Expected an unsigned integer");
        errno = 0;
        x = strtoul(text, &end, 10);
        if (!len || errno || *end || x > UINT_MAX) return fail(err, cap, "Unsigned integer is out of range");
        *(unsigned *)target = (unsigned)x;
    }
    if (!field_valid(&next, f)) {
        if(f->kind==ENV_NAME)
            return fail(err,cap,"token_env expects an environment variable name, e.g. TB_TOKEN; set the token in that variable before launching.");
        if (err && cap) (void)snprintf(err, cap, "Invalid configuration field: %s", f->name);
        return 0;
    }
    *c = next;
    return 1;
}

int cfg_validate(const tb_config *c, unsigned max_windows, char *err, size_t cap)
{
    size_t i;
    clear_error(err, cap);
    if (!c || !max_windows) return fail(err, cap, "Invalid configuration arguments");
    for (i = 0; i < FIELD_COUNT; ++i)
        if (!field_valid(c, &fields[i])) {
            if (err && cap) (void)snprintf(err, cap, "Invalid configuration field: %s", fields[i].name);
            return 0;
        }
    if (c->max_windows > max_windows) return fail(err, cap, "max_windows exceeds the compiled queue limit");
    if(c->udp_backup_enabled) {
        char host[sizeof(c->udp_backup_host)];
        unsigned port;
        if(!cfg_udp_backup_destination(c,host,sizeof(host),&port,err,cap)) return 0;
    }
    return 1;
}

#ifdef _WIN32
static int wide_path(const char *path, wchar_t out[CFG_PATH_BYTES])
{
    return path && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, out, CFG_PATH_BYTES) > 0;
}
static FILE *read_file(const char *path)
{
    wchar_t wide[CFG_PATH_BYTES];
    return wide_path(path, wide) ? _wfopen(wide, L"rb") : NULL;
}
static void remove_file(const char *path)
{
    wchar_t wide[CFG_PATH_BYTES];
    if (wide_path(path, wide)) (void)DeleteFileW(wide);
}
static FILE *exclusive_file(const char *path)
{
    wchar_t wide[CFG_PATH_BYTES];
    HANDLE handle;
    int fd;
    FILE *file;
    if (!wide_path(path, wide)) { errno = EINVAL; return NULL; }
    handle = CreateFileW(wide, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        errno = error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS ? EEXIST : EACCES;
        return NULL;
    }
    fd = _open_osfhandle((intptr_t)handle, _O_WRONLY | _O_BINARY);
    if (fd < 0) { CloseHandle(handle); (void)DeleteFileW(wide); return NULL; }
    file = _fdopen(fd, "wb");
    if (!file) { _close(fd); (void)DeleteFileW(wide); }
    return file;
}
static int sync_file(FILE *file) { return _commit(_fileno(file)) == 0; }
static unsigned long next_counter(void)
{
    static volatile LONG counter;
    return (unsigned long)InterlockedIncrement(&counter);
}
static unsigned long process_id(void) { return (unsigned long)GetCurrentProcessId(); }
static int replace_file(const char *temporary, const char *path)
{
    wchar_t from[CFG_PATH_BYTES], to[CFG_PATH_BYTES];
    return wide_path(temporary, from) && wide_path(path, to) &&
        MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}
#else
static FILE *read_file(const char *path) { return fopen(path, "rb"); }
static void remove_file(const char *path) { (void)unlink(path); }
static FILE *exclusive_file(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
    FILE *file;
    if (fd < 0) return NULL;
    file = fdopen(fd, "wb");
    if (!file) { int saved = errno; close(fd); unlink(path); errno = saved; }
    return file;
}
static int sync_file(FILE *file) { return fsync(fileno(file)) == 0; }
static unsigned long next_counter(void)
{
    static unsigned long counter;
    return __sync_add_and_fetch(&counter, 1ul);
}
static unsigned long process_id(void) { return (unsigned long)getpid(); }
static int replace_file(const char *temporary, const char *path) { return rename(temporary, path) == 0; }
#endif

static char *trim(char *text)
{
    char *last;
    while (space((unsigned char)*text)) ++text;
    last = text + strlen(text);
    while (last > text && space((unsigned char)last[-1])) --last;
    *last = '\0';
    return text;
}

/* File encodings are separate from the editor's raw credential input. Only
 * complete decrypted values reach cfg_set, which applies the shared validator. */
static const char protected_prefix[] = "dpapi:";
static const char literal_prefix[] = "plain:";
static int load_secret(tb_config *c,const char *key,char *value,char *err,size_t cap)
{
    const char *text=trim(value);
    if(!strncmp(text,literal_prefix,sizeof(literal_prefix)-1))
        return cfg_set(c,key,text+sizeof(literal_prefix)-1,err,cap);
    if(strncmp(text,protected_prefix,sizeof(protected_prefix)-1))
        return cfg_set(c,key,text,err,cap);
#ifdef _WIN32
    {
        unsigned char bytes[CFG_LINE_BYTES/2];
        DATA_BLOB encrypted,decrypted={0,NULL};
        size_t i,n;
        int ok;
        text+=sizeof(protected_prefix)-1;
        n=strlen(text);
        if(!n || n%2 || n/2>sizeof(bytes))
            return fail(err,cap,"Invalid protected token; enter the token again in settings.");
        for(i=0;i<n/2;++i) {
            int high=hex_value((unsigned char)text[2*i]);
            int low=hex_value((unsigned char)text[2*i+1]);
            if(high<0 || low<0)
                return fail(err,cap,"Invalid protected token; enter the token again in settings.");
            bytes[i]=(unsigned char)((high<<4)|low);
        }
        encrypted.cbData=(DWORD)(n/2); encrypted.pbData=bytes;
        if(!CryptUnprotectData(&encrypted,NULL,NULL,NULL,NULL,CRYPTPROTECT_UI_FORBIDDEN,&decrypted))
            return fail(err,cap,"Cannot decrypt token for this Windows user; clear token= and enter it again.");
        ok=decrypted.cbData>0 && decrypted.cbData<=CFG_AUTH_TOKEN_BYTES &&
           decrypted.pbData[decrypted.cbData-1]=='\0' &&
           !memchr(decrypted.pbData,'\0',decrypted.cbData-1);
        if(ok) ok=cfg_set(c,key,(const char *)decrypted.pbData,err,cap);
        else (void)fail(err,cap,"Invalid protected token; clear token= and enter it again.");
        SecureZeroMemory(decrypted.pbData,decrypted.cbData);
        LocalFree(decrypted.pbData);
        return ok;
    }
#else
    return fail(err,cap,"Windows-protected token cannot be used here; clear token= and enter it again or use token_env.");
#endif
}
static int save_secret(const char *token,char *out,size_t cap)
{
    if(!token[0]) { if(!cap) return 0; out[0]='\0'; return 1; }
#ifdef _WIN32
    {
        static const char hex[]="0123456789abcdef";
        DATA_BLOB plain,encrypted={0,NULL};
        size_t i,prefix=sizeof(protected_prefix)-1;
        int ok;
        plain.cbData=(DWORD)(strlen(token)+1); plain.pbData=(BYTE *)(void *)token;
        if(!CryptProtectData(&plain,L"MAVLink router HTTP token",NULL,NULL,NULL,
                             CRYPTPROTECT_UI_FORBIDDEN,&encrypted)) return 0;
        ok=cap>prefix && encrypted.cbData<=(cap-prefix-1)/2;
        if(ok) {
            memcpy(out,protected_prefix,prefix);
            for(i=0;i<encrypted.cbData;++i) {
                out[prefix+2*i]=hex[encrypted.pbData[i]>>4];
                out[prefix+2*i+1]=hex[encrypted.pbData[i]&15];
            }
            out[prefix+2*encrypted.cbData]='\0';
        }
        LocalFree(encrypted.pbData);
        return ok;
    }
#else
    /* Escape reserved file prefixes without restricting valid input tokens. */
    const char *prefix=(!strncmp(token,protected_prefix,sizeof(protected_prefix)-1) ||
                        !strncmp(token,literal_prefix,sizeof(literal_prefix)-1)) ? literal_prefix : "";
    int n=snprintf(out,cap,"%s%s",prefix,token);
    return n>=0 && (size_t)n<cap;
#endif
}

int cfg_load(const char *path, tb_config *c, unsigned max_windows, char *err, size_t cap)
{
    tb_config next;
    FILE *file;
    char line[CFG_LINE_BYTES];
    uint64_t seen = 0;
    unsigned line_number = 0;
    int ok = 1, ch = 0;
    clear_error(err, cap);
    if (!path || !*path || !c) return fail(err, cap, "Invalid configuration arguments");
    file = read_file(path);
    if (!file) return fail(err, cap, "Could not open configuration file");
    cfg_defaults(&next);
    for (;;) {
        size_t used = 0;
        char *key, *value;
        int index;
        while ((ch = fgetc(file)) != EOF && ch != '\n') {
            if (!ch || used + 1 >= sizeof(line)) {
                ok = fail(err, cap, "Configuration line is too long or contains NUL");
                break;
            }
            line[used++] = (char)ch;
        }
        if (!ok || (ch == EOF && !used)) break;
        line[used] = '\0';
        ++line_number;
        key = line;
        if (line_number == 1 && used >= 3 &&
            (unsigned char)key[0] == 0xef && (unsigned char)key[1] == 0xbb &&
            (unsigned char)key[2] == 0xbf) key += 3;
        key = trim(key);
        if (!*key || *key == '#' || *key == ';') continue;
        value = strchr(key, '=');
        if (!value) { ok = fail(err, cap, "Expected key=value configuration line"); break; }
        *value++ = '\0';
        key = trim(key);
        index = field_index(key);
        if (index < 0) { ok = fail(err, cap, "Unknown configuration key"); break; }
        if (seen & (UINT64_C(1) << index)) {
            ok = fail(err, cap, "Duplicate configuration key"); break;
        }
        seen |= UINT64_C(1) << index;
        if (!(cfg_field_is_secret((unsigned)index) ? load_secret(&next,key,value,err,cap) :
              cfg_set(&next, key, value, err, cap))) { ok = 0; break; }
    }
    if (ferror(file)) ok = fail(err, cap, "Could not read complete configuration file");
    if (fclose(file)) ok = fail(err, cap, "Could not close configuration file");
    if (!ok || !cfg_validate(&next, max_windows, err, cap)) return 0;
    *c = next;
    return 1;
}

static int write_config(FILE *file, const tb_config *c)
{
    unsigned i;
    char value[CFG_LINE_BYTES];
    if (fprintf(file, "# Restart to apply. Use a unique session for every boot/run.\n") < 0) return 0;
    for (i = 0; i < cfg_field_count(); ++i) {
        const char *raw=(const char *)c+fields[i].offset;
        if (!(cfg_field_is_secret(i) ? save_secret(raw,value,sizeof(value)) :
              cfg_field_value(c,i,value,sizeof(value),1)) ||
            fprintf(file,"%s=%s\n",cfg_field_name(i),value)<0) return 0;
    }
    return !ferror(file);
}

int cfg_save(const char *path, const tb_config *c, unsigned max_windows, char *err, size_t cap)
{
    char temporary[CFG_PATH_BYTES];
    FILE *file = NULL;
    unsigned attempt;
    int written, ok;
    clear_error(err, cap);
    if (!path || !*path) return fail(err, cap, "Invalid configuration path");
    if (!cfg_validate(c, max_windows, err, cap)) return 0;
    for (attempt = 0; attempt < 32; ++attempt) {
        written = snprintf(temporary, sizeof(temporary), "%s.tmp.%lu.%lu",
                           path, process_id(), next_counter());
        if (written < 0 || (size_t)written >= sizeof(temporary))
            return fail(err, cap, "Configuration path is too long");
        file = exclusive_file(temporary);
        if (file || errno != EEXIST) break;
    }
    if (!file) return fail(err, cap, "Could not create sibling temporary configuration file");
#ifndef _WIN32
    if (c->token[0]) {
        struct stat metadata;
        if (fstat(fileno(file), &metadata) || !S_ISREG(metadata.st_mode) ||
            metadata.st_uid != geteuid() ||
            (metadata.st_mode & 0777) != (S_IRUSR | S_IWUSR)) {
            (void)fclose(file);
            remove_file(temporary);
            return fail(err, cap,
                "Direct token requires owner-only (0600) file permissions; original file preserved");
        }
    }
#endif
    ok = write_config(file, c);
    if (ok && fflush(file)) ok = 0;
    if (ok && !sync_file(file)) ok = 0;
    if (fclose(file)) ok = 0;
    if (!ok) {
        remove_file(temporary);
        return fail(err, cap, "Could not write complete configuration; original file preserved");
    }
    if (!replace_file(temporary, path)) {
        remove_file(temporary);
        return fail(err, cap, "Could not replace configuration; original file preserved");
    }
    return 1;
}

enum cfg_url_display { URL_DISPLAY_ACTIVE, URL_DISPLAY_DRAFT };

/* Recognize an authority boundary without requiring a valid scheme. This
 * preserves a partially edited prefix such as ttp://, http:/ or http// while
 * preventing its userinfo from becoming visible when one character is erased.
 * Without an identifiable prefix, redact everything before the last @. */
static const char *draft_authority(const char *url,const char *end)
{
    const char *p=url;
    while(p<end && (letter((unsigned char)*p) || digit((unsigned char)*p) ||
                   *p=='+' || *p=='-' || *p=='.')) ++p;
    if(p+1<end && *p==':' && p[1]=='/') ++p;
    else if(!(p+1<end && *p=='/' && p[1]=='/')) return url;
    while(p<end && *p=='/') ++p;
    return p;
}

static void format_url(const char *url,char *out,size_t cap,enum cfg_url_display mode)
{
    const char *cut, *p, *authority, *authority_end, *at = NULL;
    size_t used = 0;
    if (!out || !cap) return;
    if (!url) { out[0] = '\0'; return; }
    if (mode==URL_DISPLAY_ACTIVE && *url && strcmp(url, "-") && strncmp(url, "http://", 7) && strncmp(url, "https://", 8) &&
        strncmp(url, "ws://", 5) && strncmp(url, "wss://", 6)) {
        (void)snprintf(out, cap, "[invalid URL]");
        return;
    }
    cut = url + strcspn(url, "?#");
    authority = mode==URL_DISPLAY_DRAFT ? draft_authority(url,cut) : strstr(url, "://");
    if (authority) {
        if(mode==URL_DISPLAY_ACTIVE) authority += 3;
        authority_end = authority==url ? cut : authority + strcspn(authority, "/?#");
        for (p = authority; p < authority_end; ++p) if (*p == '@') at = p;
    }
    for (p = url; p < cut && used + 1 < cap; ++p) {
        unsigned char ch;
        if (at && p == authority) p = at + 1;
        if (p >= cut) break;
        ch = (unsigned char)*p;
        out[used++] = ch <= 32 || ch == 127 ? '_' : (char)ch;
    }
    out[used] = '\0';
}

void cfg_safe_url(const char *url,char *out,size_t cap)
{ format_url(url,out,cap,URL_DISPLAY_ACTIVE); }

void cfg_edit_url(const char *url,char *out,size_t cap)
{ format_url(url,out,cap,URL_DISPLAY_DRAFT); }
