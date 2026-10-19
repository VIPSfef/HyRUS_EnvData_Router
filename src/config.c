#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "config.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define CFG_PATH_BYTES 4096
#define CFG_LINE_BYTES 4096
enum { URL, TOKEN, ENV_NAME, INPUT_MODE, SERIAL_PORT, WEBSOCKET_URL, UNIT_MAP,
       REAL, UINT_VALUE, CFG_BOOL, BAUD_VALUE };
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
    NUM_FIELD("origin_auto", CFG_BOOL, origin_auto, 0, 1)
};
#define FIELD_COUNT (sizeof(fields) / sizeof(fields[0]))

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

static int token_valid(const char *s, int env_name)
{
    size_t i, n = strlen(s);
    if (!n || n > 64 || (env_name && !letter((unsigned char)s[0]) && s[0] != '_')) return 0;
    for (i = 0; i < n; ++i)
        if (!letter((unsigned char)s[i]) && !digit((unsigned char)s[i]) &&
            s[i] != '_' && (env_name || (s[i] != '-' && s[i] != '.'))) return 0;
    return 1;
}

static int port_valid(const char *p, const char *end)
{
    unsigned port = 0;
    if (p == end) return 0;
    while (p < end) {
        if (!digit((unsigned char)*p) || port > 6553) return 0;
        port = port * 10 + (unsigned)(*p++ - '0');
        if (port > 65535) return 0;
    }
    return port != 0;
}

static int url_valid(const char *url, int websocket)
{
    const char *host, *end, *p, *port = NULL;
    size_t i;
    if (websocket) {
        if (!*url) return 1;
        if (!strncmp(url, "ws://", 5)) host = url + 5;
        else if (!strncmp(url, "wss://", 6)) host = url + 6;
        else return 0;
    } else {
        if (!strcmp(url, "-")) return 1;
        if (!strncmp(url, "http://", 7)) host = url + 7;
        else if (!strncmp(url, "https://", 8)) host = url + 8;
        else return 0;
    }
    for (i = 0; url[i]; ++i)
        if ((unsigned char)url[i] <= 32 || (unsigned char)url[i] == 127 ||
            url[i] == '\\' || url[i] == '#') return 0;
    end = host + strcspn(host, "/?");
    if (host == end) return 0;
    if (*host == '[') {
        const char *close = memchr(host, ']', (size_t)(end - host));
        if (!close || close == host + 1) return 0;
        for (p = host + 1; p < close; ++p)
            if (!digit((unsigned char)*p) && *p != ':' && *p != '.' &&
                !(*p >= 'a' && *p <= 'f') && !(*p >= 'A' && *p <= 'F')) return 0;
        if (close + 1 != end) {
            if (close[1] != ':') return 0;
            port = close + 2;
        }
    } else {
        for (p = host; p < end && *p != ':'; ++p)
            if (!letter((unsigned char)*p) && !digit((unsigned char)*p) &&
                *p != '.' && *p != '-' && *p != '_') return 0;
        if (p == host) return 0;
        if (p < end) port = p + 1;
    }
    return !port || port_valid(port, end);
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
static int baud_valid(unsigned baud)
{
    static const unsigned supported[] = {9600,19200,38400,57600,115200,230400,460800,921600};
    size_t i;
    for (i = 0; i < sizeof(supported)/sizeof(supported[0]); ++i)
        if (baud == supported[i]) return 1;
    return 0;
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
        return token_valid(p, f->kind == ENV_NAME);
    }
    if (f->kind == REAL) {
        double x = *(const double *)p;
        return isfinite(x) && x >= f->low && x <= f->high;
    }
    if (f->kind == CFG_BOOL) return *(const int *)p == 0 || *(const int *)p == 1;
    if (f->kind == BAUD_VALUE) return baud_valid(*(const unsigned *)p);
    return *(const unsigned *)p >= f->low && *(const unsigned *)p <= f->high;
}

void cfg_defaults(tb_config *c)
{
    if (!c) return;
    memset(c, 0, sizeof(*c));
    strcpy(c->token_env, "TB_TOKEN");
    c->send_interval_ms = c->refresh_ms = 1000;
    c->connect_timeout_ms = c->io_timeout_ms = 1000;
    c->max_windows = 4;
    c->idle_warn_ms = 2000;
    c->ack_warn_ms = 5000;
    strcpy(c->input_mode, "mavlink");
    strcpy(c->serial_port, "auto");
    c->serial_baud = 115200;
    c->auto_scan = 1;
    c->origin_auto = 1;
    c->probe_ms = 2000;
    c->link_timeout_ms = 5000;
    c->reconnect_ms = 1000;
    c->system_time_uncert_us = UINT32_MAX;
    c->accel_bound_mps2 = 10;
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
    while (space((unsigned char)*value)) ++value;
    last = value + strlen(value);
    while (last > value && space((unsigned char)last[-1])) --last;
    len = (size_t)(last - value);
    if (len >= sizeof(text)) return fail(err, cap, "Configuration value is too long");
    memcpy(text, value, len); text[len] = '\0';
    next = *c;
    f = &fields[index];
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
        if (!cfg_set(&next, key, value, err, cap)) { ok = 0; break; }
    }
    if (ferror(file)) ok = fail(err, cap, "Could not read complete configuration file");
    if (fclose(file)) ok = fail(err, cap, "Could not close configuration file");
    if (!ok || !cfg_validate(&next, max_windows, err, cap)) return 0;
    *c = next;
    return 1;
}

static int write_config(FILE *file, const tb_config *c)
{
    size_t i;
    if (fprintf(file, "# Restart to apply. Use a unique session for every boot/run.\n") < 0) return 0;
    for (i = 0; i < FIELD_COUNT; ++i) {
        const cfg_field *f = &fields[i];
        const char *p = (const char *)c + f->offset;
        int result;
        if (f->kind < REAL) result = fprintf(file, "%s=%s\n", f->name, p);
        else if (f->kind == REAL) result = fprintf(file, "%s=%.17g\n", f->name, *(const double *)p);
        else if (f->kind == CFG_BOOL) result = fprintf(file, "%s=%d\n", f->name, *(const int *)p);
        else result = fprintf(file, "%s=%u\n", f->name, *(const unsigned *)p);
        if (result < 0) return 0;
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

void cfg_safe_url(const char *url, char *out, size_t cap)
{
    const char *cut, *p, *authority, *authority_end, *at = NULL;
    size_t used = 0;
    if (!out || !cap) return;
    if (!url) { out[0] = '\0'; return; }
    if (*url && strcmp(url, "-") && strncmp(url, "http://", 7) && strncmp(url, "https://", 8) &&
        strncmp(url, "ws://", 5) && strncmp(url, "wss://", 6)) {
        (void)snprintf(out, cap, "[invalid URL]");
        return;
    }
    cut = url + strcspn(url, "?#");
    authority = strstr(url, "://");
    if (authority) {
        authority += 3;
        authority_end = authority + strcspn(authority, "/?#");
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
