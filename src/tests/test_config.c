/* Independent strict configuration parsing, boundaries and file transaction tests. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "config.h"

#include <locale.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test_workdir.h"
#ifdef _WIN32
#include <windows.h>
#include <process.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); return 0; \
} } while (0)

static FILE *open_utf8(const char *path, const char *mode)
{
#ifdef _WIN32
    wchar_t wide[1024], wide_mode[16];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, 1024) ||
        !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, mode, -1, wide_mode, 16)) return NULL;
    return _wfopen(wide, wide_mode);
#else
    return fopen(path, mode);
#endif
}

static int remove_utf8(const char *path)
{
#ifdef _WIN32
    wchar_t wide[1024];
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, 1024) &&
           DeleteFileW(wide);
#else
    return remove(path) == 0;
#endif
}

static unsigned process_id(void)
{
#ifdef _WIN32
    return (unsigned)_getpid();
#else
    return (unsigned)getpid();
#endif
}

static int write_bytes(const char *path, const char *bytes, size_t size)
{
    FILE *f = open_utf8(path, "wb");
    int ok;
    if (!f) return 0;
    ok = fwrite(bytes, 1, size, f) == size;
    return fclose(f) == 0 && ok;
}

static int file_equals(const char *path, const char *expected)
{
    char actual[8192];
    FILE *f = open_utf8(path, "rb");
    size_t n;
    int ok;
    if (!f) return 0;
    n = fread(actual, 1, sizeof(actual), f);
    ok = !ferror(f) && n == strlen(expected) && !memcmp(actual, expected, n);
    return fclose(f) == 0 && ok;
}

static int valid_config(tb_config *c)
{
    char error[128];
    cfg_defaults(c);
    return cfg_set(c, "url", "http://127.0.0.1:14550/ingest?api_key=PRIVATE_QUERY", error, sizeof(error)) &&
           cfg_set(c, "stream", "sensor-observations", error, sizeof(error)) &&
           cfg_set(c, "session", "session-20261019", error, sizeof(error)) &&
           cfg_validate(c, 4, error, sizeof(error));
}

static int invalid_set(tb_config *c, const char *key, const char *value)
{
    tb_config saved = *c;
    char error[128] = "";
    CHECK(!cfg_set(c, key, value, error, sizeof(error)));
    CHECK(!memcmp(&saved, c, sizeof(saved)));
    CHECK(error[0] != '\0');
    if (strlen(value) >= 10) CHECK(strstr(error, value) == NULL);
    return 1;
}

static int check_settings(void)
{
    tb_config c, saved;
    char error[128];
    const char *timeout_keys[] = {"connect_timeout_ms", "io_timeout_ms"};
    const char *warn_keys[] = {"idle_warn_ms", "ack_warn_ms"};
    size_t i;
    cfg_defaults(&c);
    CHECK(!cfg_validate(&c, 4, error, sizeof(error)));
    CHECK(c.send_interval_ms >= 1000);
    CHECK(c.http_min_interval_ms == 500);
    CHECK(c.refresh_ms >= 250 && c.refresh_ms <= 10000);
    CHECK(!strcmp(c.token_env, "TB_TOKEN"));
    CHECK(valid_config(&c));
    CHECK(cfg_set(&c, "origin_lat", "37.12345678912345", error, sizeof(error)));
    CHECK(cfg_set(&c, "origin_lon", "-127.12345678912345", error, sizeof(error)));
    CHECK(cfg_set(&c, "origin_alt", "123.125", error, sizeof(error)));
    CHECK(cfg_set(&c, "token_env", "TB_TEST_PRIVATE_TOKEN", error, sizeof(error)));
    CHECK(cfg_set(&c, "summary", "1", error, sizeof(error)) && c.summary == 1);
    CHECK(cfg_set(&c, "summary", "0", error, sizeof(error)) && c.summary == 0);
    CHECK(invalid_set(&c, "url", "ftp://127.0.0.1/PRIVATE_BAD_SCHEME"));
    CHECK(invalid_set(&c, "url", "http://PRIVATE_USER:PRIVATE_PASSWORD@example.com/x"));
    CHECK(invalid_set(&c, "url", "http://example.com/x#PRIVATE_FRAGMENT"));
    CHECK(invalid_set(&c, "url", "http://example.com/x PRIVATE_SPACE"));
    CHECK(invalid_set(&c, "url", "http://example.com/x\r\nPRIVATE_HEADER"));
    CHECK(invalid_set(&c, "stream", "invalid stream"));
    CHECK(invalid_set(&c, "session", "invalid\nsession"));
    CHECK(invalid_set(&c, "token_env", "Bearer PRIVATE_TOKEN_VALUE"));
    CHECK(!cfg_set(&c, "token_env", "0123456789abcdef", error, sizeof(error)));
    CHECK(strstr(error,"environment variable name")!=NULL && strstr(error,"TB_TOKEN")!=NULL);
    CHECK(strstr(error,"0123456789abcdef")==NULL);
    CHECK(cfg_set(&c, "token", "PRIVATE_TOKEN_VALUE", error, sizeof(error)));
    CHECK(cfg_set(&c, "token", "", error, sizeof(error)));
    CHECK(invalid_set(&c, "origin_lat", "90.0001"));
    CHECK(invalid_set(&c, "origin_lon", "-180.0001"));
    CHECK(invalid_set(&c, "origin_alt", "nan"));
    CHECK(invalid_set(&c, "origin_lat", "inf"));
    CHECK(invalid_set(&c, "summary", "2"));
    CHECK(invalid_set(&c, "summary", "true"));
    CHECK(invalid_set(&c, "send_interval_ms", "999"));
    CHECK(invalid_set(&c, "send_interval_ms", "-1"));
    CHECK(invalid_set(&c, "send_interval_ms", "4294967296"));
    CHECK(invalid_set(&c, "send_interval_ms", "1000garbage"));
    CHECK(cfg_set(&c, "send_interval_ms", "1000", error, sizeof(error)));
    CHECK(invalid_set(&c, "http_min_interval_ms", "499"));
    CHECK(invalid_set(&c, "http_min_interval_ms", "0"));
    CHECK(invalid_set(&c, "http_min_interval_ms", "60001"));
    CHECK(cfg_set(&c, "http_min_interval_ms", "60000", error, sizeof(error)));
    CHECK(cfg_set(&c, "http_min_interval_ms", "500", error, sizeof(error)));
    for (i = 0; i < sizeof(timeout_keys) / sizeof(timeout_keys[0]); ++i) {
        CHECK(invalid_set(&c, timeout_keys[i], "99"));
        CHECK(cfg_set(&c, timeout_keys[i], "100", error, sizeof(error)));
        CHECK(cfg_set(&c, timeout_keys[i], "60000", error, sizeof(error)));
        CHECK(invalid_set(&c, timeout_keys[i], "60001"));
    }
    CHECK(invalid_set(&c, "refresh_ms", "249"));
    CHECK(cfg_set(&c, "refresh_ms", "250", error, sizeof(error)));
    CHECK(cfg_set(&c, "refresh_ms", "10000", error, sizeof(error)));
    CHECK(invalid_set(&c, "refresh_ms", "10001"));
    for (i = 0; i < sizeof(warn_keys) / sizeof(warn_keys[0]); ++i) {
        CHECK(invalid_set(&c, warn_keys[i], "249"));
        CHECK(cfg_set(&c, warn_keys[i], "250", error, sizeof(error)));
        CHECK(cfg_set(&c, warn_keys[i], "3600000", error, sizeof(error)));
        CHECK(invalid_set(&c, warn_keys[i], "3600001"));
    }
    CHECK(invalid_set(&c, "max_windows", "0"));
    CHECK(cfg_set(&c, "max_windows", "4", error, sizeof(error)));
    saved = c;
    CHECK(!cfg_validate(&c, 3, error, sizeof(error)));
    CHECK(!memcmp(&saved, &c, sizeof(c)));
    CHECK(cfg_validate(&c, 4, error, sizeof(error)));
    return 1;
}

static int check_field_registry(void)
{
    tb_config original,roundtrip;
    char raw[2048],display[2048],error[128],tiny[2];
    unsigned i,urls=0,count=cfg_field_count();
    CHECK(valid_config(&original));
    CHECK(cfg_set(&original,"websocket_url","wss://example.com/data?key=PRIVATE_WS",error,sizeof(error)));
    CHECK(cfg_set(&original,"origin_lat","37.12345678912345",error,sizeof(error)));
    CHECK(count>0 && count<=64);
    CHECK(!strcmp(cfg_field_name(7),"send_interval_ms"));
    CHECK(!strcmp(cfg_field_name(11),"refresh_ms"));
    CHECK(!strcmp(cfg_field_name(31),"udp_port"));
    CHECK(!strcmp(cfg_field_name(36),"spatial_adaptive"));
    CHECK(!strcmp(cfg_field_name(37),"qgc_enabled"));
    CHECK(!strcmp(cfg_field_name(38),"qgc_host"));
    CHECK(!strcmp(cfg_field_name(39),"qgc_port"));
    CHECK(!strcmp(cfg_field_name(40),"status_interval_ms"));
    CHECK(!strcmp(cfg_field_name(41),"mavlink_rates_enabled"));
    CHECK(!strcmp(cfg_field_name(42),"udp_backup_enabled"));
    CHECK(!strcmp(cfg_field_name(43),"udp_backup_host"));
    CHECK(!strcmp(cfg_field_name(44),"udp_backup_port"));
    CHECK(!strcmp(cfg_field_name(45),"tcp_buffer_max_mb"));
    CHECK(!strcmp(cfg_field_name(46),"token") && cfg_field_is_secret(46));
    CHECK(!strcmp(cfg_field_name(47),"http_min_interval_ms"));
    cfg_defaults(&roundtrip);
    for(i=0;i<count;++i) {
        const char *name=cfg_field_name(i);
        CHECK(name && *name);
        CHECK(cfg_field_value(&original,i,raw,sizeof(raw),1));
        CHECK(cfg_set(&roundtrip,name,raw,error,sizeof(error)));
        CHECK(cfg_field_value(&original,i,display,sizeof(display),0));
        if(cfg_field_is_url(i)) {
            ++urls;
            CHECK(strchr(raw,'?')!=NULL && strchr(display,'?')==NULL);
            CHECK(strstr(display,"PRIVATE")==NULL);
        } else CHECK(!strcmp(raw,display));
    }
    CHECK(urls==2 && !memcmp(&original,&roundtrip,sizeof(original)));
    CHECK(cfg_field_name(count)==NULL && !cfg_field_is_url(count) && !cfg_field_is_secret(count));
    CHECK(!cfg_field_value(&original,count,raw,sizeof(raw),1) && !raw[0]);
    CHECK(!cfg_field_value(NULL,0,raw,sizeof(raw),1) && !raw[0]);
    CHECK(!cfg_field_value(&original,0,NULL,0,1));
    CHECK(!cfg_field_value(&original,0,tiny,sizeof(tiny),1) && tiny[1]=='\0');
    CHECK(!cfg_field_value(&original,0,tiny,sizeof(tiny),0) && tiny[1]=='\0');
    return 1;
}

static int check_tcp_buffer_settings(void)
{
    tb_config c, loaded;
    char error[128], text[32], path[128];
    unsigned max_mb = SIZE_MAX / CFG_MIB_BYTES < CFG_TCP_BUFFER_MAX_MB ?
        (unsigned)(SIZE_MAX / CFG_MIB_BYTES) : CFG_TCP_BUFFER_MAX_MB;
    CHECK(valid_config(&c));
    CHECK(c.tcp_buffer_max_mb == 1024);
    CHECK(invalid_set(&c,"tcp_buffer_max_mb","0"));
    CHECK(invalid_set(&c,"tcp_buffer_max_mb","-1"));
    CHECK(invalid_set(&c,"tcp_buffer_max_mb","1.5"));
    CHECK(cfg_set(&c,"tcp_buffer_max_mb","1",error,sizeof(error)));
    CHECK(c.tcp_buffer_max_mb == 1);
    (void)snprintf(text,sizeof(text),"%u",max_mb);
    CHECK(cfg_set(&c,"tcp_buffer_max_mb",text,error,sizeof(error)));
    CHECK(c.tcp_buffer_max_mb == max_mb);
    CHECK(cfg_validate(&c,4,error,sizeof(error)));
    (void)snprintf(text,sizeof(text),"%u",max_mb+1);
    CHECK(invalid_set(&c,"tcp_buffer_max_mb",text));
    c.tcp_buffer_max_mb = max_mb+1;
    CHECK(!cfg_validate(&c,4,error,sizeof(error)));
    CHECK(cfg_set(&c,"tcp_buffer_max_mb","2048",error,sizeof(error)));
    (void)snprintf(path,sizeof(path),"test_tcp_buffer_%u.ini",process_id());
    CHECK(cfg_save(path,&c,4,error,sizeof(error)));
    cfg_defaults(&loaded);
    CHECK(cfg_load(path,&loaded,4,error,sizeof(error)));
    CHECK(loaded.tcp_buffer_max_mb == 2048);
    CHECK(!memcmp(&loaded,&c,sizeof(c)));
    CHECK(remove_utf8(path));
    return 1;
}

static int check_redaction(void)
{
    char safe[4096];
    struct { unsigned char left[8]; char value[8]; unsigned char right[8]; } guard;
    size_t i, cap;
    const char *url = "https://example.com:443/path?access_token=PRIVATE_QUERY#PRIVATE_FRAGMENT";
    cfg_safe_url(url, safe, sizeof(safe));
    CHECK(!strstr(safe, "PRIVATE_QUERY") && !strstr(safe, "PRIVATE_FRAGMENT"));
    CHECK(!strchr(safe, '?') && !strchr(safe, '#'));
    CHECK(strstr(safe, "example.com") && strstr(safe, "/path"));
    cfg_safe_url("http://PRIVATE_USER:PRIVATE_PASSWORD@example.com/x?secret=PRIVATE_QUERY",
                 safe, sizeof(safe));
    CHECK(!strstr(safe, "PRIVATE_USER") && !strstr(safe, "PRIVATE_PASSWORD"));
    CHECK(!strstr(safe, "PRIVATE_QUERY"));
    for (cap = 0; cap <= sizeof(guard.value); ++cap) {
        memset(&guard, 0xA5, sizeof(guard));
        cfg_safe_url(url, guard.value, cap);
        for (i = 0; i < sizeof(guard.left); ++i) CHECK(guard.left[i] == 0xA5);
        for (i = cap; i < sizeof(guard.value); ++i)
            CHECK((unsigned char)guard.value[i] == 0xA5);
        for (i = 0; i < sizeof(guard.right); ++i) CHECK(guard.right[i] == 0xA5);
        if (cap) CHECK(memchr(guard.value, '\0', cap) != NULL);
    }
    return 1;
}

static int check_url_drafts(void)
{
    static const struct { const char *input,*shown; } cases[] = {
        {"", ""}, {"-", "-"}, {"h", "h"}, {"ht", "ht"}, {"http", "http"},
        {"http:", "http:"}, {"http:/", "http:/"}, {"http://", "http://"},
        {"http://127.0.0.1:", "http://127.0.0.1:"},
        {"ttp://example.com/ingest", "ttp://example.com/ingest"},
        {"w", "w"}, {"ws:/", "ws:/"}, {"wss://", "wss://"},
        {"http://PRIVATE_USER:PRIVATE_PASSWORD@example.com/x?PRIVATE_QUERY#PRIVATE_FRAGMENT", "http://example.com/x"},
        {"ttp://PRIVATE_USER:PRIVATE_PASSWORD@example.com/x?PRIVATE_QUERY", "ttp://example.com/x"},
        {"http//PRIVATE_USER:PRIVATE_PASSWORD@example.com/x#PRIVATE_FRAGMENT", "http//example.com/x"},
        {"http:/PRIVATE_USER:PRIVATE_PASSWORD@example.com/x", "http:/example.com/x"},
        {"//PRIVATE_USER:PRIVATE_PASSWORD@example.com/x", "//example.com/x"},
        {"PRIVATE_USER:PRIVATE_PASSWORD@example.com/x", "example.com/x"},
        {"PRIVATE_USER:PRIVATE_PASSWORD@PRIVATE_OTHER@example.com/x", "example.com/x"},
        {"h?PRIVATE_QUERY#PRIVATE_FRAGMENT", "h"},
        {"http://host/path@name", "http://host/path@name"},
        {"http://ho\nst/\033path", "http://ho_st/_path"}
    };
    struct { unsigned char left[8]; char value[8]; unsigned char right[8]; } guard;
    tb_config c;
    char shown[512];
    size_t item,cap,i;
    cfg_defaults(&c);
    for(item=0;item<sizeof(cases)/sizeof(cases[0]);++item) {
        cfg_edit_url(cases[item].input,shown,sizeof(shown));
        CHECK(!strcmp(shown,cases[item].shown));
        CHECK(!strstr(shown,"PRIVATE_"));
        for(cap=0;cap<=sizeof(guard.value);++cap) {
            memset(&guard,0xA5,sizeof(guard));
            cfg_edit_url(cases[item].input,guard.value,cap);
            for(i=0;i<sizeof(guard.left);++i) CHECK(guard.left[i]==0xA5);
            for(i=cap;i<sizeof(guard.value);++i) CHECK((unsigned char)guard.value[i]==0xA5);
            for(i=0;i<sizeof(guard.right);++i) CHECK(guard.right[i]==0xA5);
            if(cap) CHECK(memchr(guard.value,'\0',cap)!=NULL);
        }
    }
    cfg_safe_url("h",shown,sizeof(shown)); CHECK(!strcmp(shown,"[invalid URL]"));
    cfg_safe_url("http//host/x",shown,sizeof(shown)); CHECK(!strcmp(shown,"[invalid URL]"));
    cfg_edit_url(NULL,shown,sizeof(shown)); CHECK(!shown[0]);
    cfg_edit_url("h",NULL,sizeof(shown));
    CHECK(invalid_set(&c,"url","http://"));
    CHECK(invalid_set(&c,"websocket_url","ws://"));
    return 1;
}

static int invalid_file(const char *path, const char *bytes, size_t size, tb_config *c)
{
    tb_config saved = *c;
    char error[128] = "";
    CHECK(write_bytes(path, bytes, size));
    CHECK(!cfg_load(path, c, 4, error, sizeof(error)));
    CHECK(!memcmp(&saved, c, sizeof(saved)));
    CHECK(error[0] != '\0');
    CHECK(strstr(error, "PRIVATE_FILE_VALUE") == NULL);
    return 1;
}

static int check_files(void)
{
    const char valid[] =
        "\xEF\xBB\xBF# UTF-8 configuration\r\n"
        "; comment\r\n\r\n"
        "url = https://example.com/ingest?key=PRIVATE_FILE_VALUE\r\n"
        "stream = weather.test\r\n"
        "session = flight-1\r\n"
        "token_env = TB_TEST_PRIVATE_TOKEN\r\n"
        "origin_lat = 37.12345678912345\r\n"
        "origin_lon = 127.125\r\norigin_alt = 123.125\r\n"
        "send_interval_ms = 1000\r\nconnect_timeout_ms = 100\r\n"
        "io_timeout_ms = 60000\r\nmax_windows = 4\r\nrefresh_ms = 250\r\n"
        "idle_warn_ms = 3000\r\nack_warn_ms = 4000\r\nsummary = 1\r\n";
    const char duplicate[] = "url=http://example.com/x\nstream=x\nsession=y\nurl=http://example.com/z\n";
    const char unknown[] = "url=http://example.com/x\nstream=x\nsession=y\nunknown=PRIVATE_FILE_VALUE\n";
    const char missing[] = "url=http://example.com/x\nstream=x\n";
    const char nul[] = "url=http://example.com/x\nstream=x\nsession=y\0PRIVATE_FILE_VALUE\n";
    char path[256], unicode_path[256], absent_path[256], long_line[5000], error[128];
    tb_config c, saved, loaded;
    FILE *f;
    (void)snprintf(path, sizeof(path), "test_config_%u.ini", process_id());
    (void)snprintf(unicode_path, sizeof(unicode_path),
                   "test_config_\xED\x95\x9C\xEA\xB8\x80_%u.ini", process_id());
    (void)snprintf(absent_path, sizeof(absent_path),
                   "test_config_absent_directory_%u/config.ini", process_id());
    CHECK(valid_config(&c));
    CHECK(invalid_file(path, duplicate, sizeof(duplicate) - 1, &c));
    CHECK(invalid_file(path, unknown, sizeof(unknown) - 1, &c));
    CHECK(invalid_file(path, missing, sizeof(missing) - 1, &c));
    CHECK(invalid_file(path, nul, sizeof(nul) - 1, &c));
    memset(long_line, 'a', sizeof(long_line));
    CHECK(invalid_file(path, long_line, sizeof(long_line), &c));
    CHECK(write_bytes(path, valid, sizeof(valid) - 1));
    CHECK(cfg_load(path, &c, 4, error, sizeof(error)));
    CHECK(!strcmp(c.stream, "weather.test") && !strcmp(c.session, "flight-1"));
    CHECK(c.origin[0] == 37.12345678912345 && c.origin[1] == 127.125);
    CHECK(c.summary == 1 && c.max_windows == 4 && c.refresh_ms == 250);
    CHECK(c.connect_timeout_ms == 100 && c.io_timeout_ms == 60000);
    CHECK(!strcmp(c.token_env, "TB_TEST_PRIVATE_TOKEN"));
    CHECK(cfg_save(unicode_path, &c, 4, error, sizeof(error)));
    cfg_defaults(&loaded);
    CHECK(cfg_load(unicode_path, &loaded, 4, error, sizeof(error)));
    CHECK(!memcmp(&loaded, &c, sizeof(c)));
    CHECK(cfg_set(&c, "session", "replaced-session", error, sizeof(error)));
    CHECK(cfg_save(unicode_path, &c, 4, error, sizeof(error)));
    CHECK(cfg_load(unicode_path, &loaded, 4, error, sizeof(error)));
    CHECK(!strcmp(loaded.session, "replaced-session"));
    /* Validation failure cannot truncate or replace the existing destination. */
    CHECK(write_bytes(path, "original sentinel", strlen("original sentinel")));
    saved = c; c.summary = 2;
    CHECK(!cfg_save(path, &c, 4, error, sizeof(error)));
    CHECK(file_equals(path, "original sentinel"));
    c = saved;
    CHECK(!cfg_save(absent_path, &c, 4, error, sizeof(error)));
    /* Failed open preserves an existing in-memory configuration. */
    saved = c;
    CHECK(!cfg_load(absent_path, &c, 4, error, sizeof(error)));
    CHECK(!memcmp(&saved, &c, sizeof(c)));
    f = open_utf8(unicode_path, "rb");
    CHECK(f != NULL);
    if (f) {
        char text[8192];
        size_t n = fread(text, 1, sizeof(text) - 1, f);
        CHECK(!ferror(f));
        text[n] = '\0';
        CHECK(strstr(text, "TB_TEST_PRIVATE_TOKEN") != NULL);
        CHECK(strstr(text, "PRIVATE_TOKEN_VALUE") == NULL);
        CHECK(fclose(f) == 0);
    }
    CHECK(remove_utf8(path));
    CHECK(remove_utf8(unicode_path));
    return 1;
}


static int check_mavlink_settings(void)
{
    tb_config c, loaded;
    char error[160], safe[2048], unit[16], path[128];
    static const unsigned baud[] = {9600,19200,38400,57600,115200,230400,460800,921600};
    size_t i;
    cfg_defaults(&c);
    CHECK(!strcmp(c.input_mode,"mavlink") && !strcmp(c.serial_port,"auto"));
    CHECK(c.serial_baud==115200 && c.auto_scan==1 && !c.websocket_url[0]);
    CHECK(c.probe_ms==2000 && c.link_timeout_ms==5000 && c.reconnect_ms==1000);
    CHECK(c.target_system==0 && c.target_component==0 && c.system_time_uncert_us==4294967295u);
    CHECK(c.accel_bound_mps2==10 && c.duration_ms==0 && !c.unit_map[0]);
    CHECK(c.origin_auto==1);
    CHECK(valid_config(&c));
    CHECK(cfg_set(&c,"input_mode","csv",error,sizeof(error)));
    CHECK(cfg_set(&c,"input_mode","mavlink",error,sizeof(error)));
    CHECK(invalid_set(&c,"input_mode","CSV"));
    CHECK(invalid_set(&c,"serial_port",""));
    CHECK(invalid_set(&c,"serial_port","/dev/ttyUSB0\nINJECT"));
    CHECK(cfg_set(&c,"serial_port","/dev/serial/by-id/controller",error,sizeof(error)));
    for(i=0;i<sizeof(baud)/sizeof(baud[0]);++i) {
        char value[32];
        (void)snprintf(value,sizeof(value),"%u",baud[i]);
        CHECK(cfg_set(&c,"serial_baud",value,error,sizeof(error)));
        CHECK(c.serial_baud==baud[i]);
    }
    CHECK(invalid_set(&c,"serial_baud","0"));
    CHECK(invalid_set(&c,"serial_baud","100000"));
    CHECK(invalid_set(&c,"websocket_url","https://example.com/socket"));
    CHECK(invalid_set(&c,"websocket_url","ws://PRIVATE_USER:PRIVATE_PASS@example.com/socket"));
    CHECK(invalid_set(&c,"websocket_url","wss://example.com/socket#PRIVATE_FRAGMENT"));
    CHECK(cfg_set(&c,"websocket_url","wss://[::1]:443/mavlink?token=PRIVATE_MAV_QUERY",error,sizeof(error)));
    cfg_safe_url(c.websocket_url,safe,sizeof(safe));
    CHECK(!strcmp(safe,"wss://[::1]:443/mavlink") && !strstr(safe,"PRIVATE_MAV_QUERY"));
    CHECK(cfg_set(&c,"auto_scan","0",error,sizeof(error)) && c.auto_scan==0);
    CHECK(cfg_set(&c,"auto_scan","1",error,sizeof(error)) && c.auto_scan==1);
    CHECK(invalid_set(&c,"auto_scan","2"));
    CHECK(invalid_set(&c,"auto_scan","true"));
    CHECK(invalid_set(&c,"origin_auto","2"));
    CHECK(invalid_set(&c,"origin_auto","true"));
    CHECK(cfg_set(&c,"origin_auto","1",error,sizeof(error)) && c.origin_auto==1);
    CHECK(cfg_set(&c,"origin_auto","0",error,sizeof(error)) && c.origin_auto==0);
    CHECK(invalid_set(&c,"probe_ms","99"));
    CHECK(cfg_set(&c,"probe_ms","100",error,sizeof(error)));
    CHECK(cfg_set(&c,"probe_ms","60000",error,sizeof(error)));
    CHECK(invalid_set(&c,"probe_ms","60001"));
    CHECK(invalid_set(&c,"link_timeout_ms","99"));
    CHECK(cfg_set(&c,"link_timeout_ms","3600000",error,sizeof(error)));
    CHECK(invalid_set(&c,"link_timeout_ms","3600001"));
    CHECK(invalid_set(&c,"reconnect_ms","99"));
    CHECK(cfg_set(&c,"reconnect_ms","60000",error,sizeof(error)));
    CHECK(invalid_set(&c,"reconnect_ms","60001"));
    CHECK(cfg_set(&c,"target_system","255",error,sizeof(error)));
    CHECK(cfg_set(&c,"target_component","191",error,sizeof(error)));
    CHECK(invalid_set(&c,"target_system","256"));
    CHECK(invalid_set(&c,"target_component","-1"));
    CHECK(cfg_set(&c,"system_time_uncert_us","4294967295",error,sizeof(error)));
    CHECK(invalid_set(&c,"system_time_uncert_us","4294967296"));
    CHECK(cfg_set(&c,"system_time_uncert_us","2500",error,sizeof(error)));
    CHECK(cfg_set(&c,"duration_ms","4294967295",error,sizeof(error)));
    CHECK(invalid_set(&c,"duration_ms","4294967296"));
    CHECK(cfg_set(&c,"accel_bound_mps2","0",error,sizeof(error)));
    CHECK(cfg_set(&c,"accel_bound_mps2","1000000",error,sizeof(error)));
    CHECK(invalid_set(&c,"accel_bound_mps2","1000000.1"));
    CHECK(invalid_set(&c,"accel_bound_mps2","nan"));
    CHECK(invalid_set(&c,"unit_map","temp:C;temp:K"));
    CHECK(invalid_set(&c,"unit_map","1bad:C"));
    CHECK(invalid_set(&c,"unit_map","temp:"));
    CHECK(invalid_set(&c,"unit_map","temp:C;humidity:%;bad:unit:extra"));
    CHECK(cfg_set(&c,"unit_map","temp:C;humidity:%;pm25:ug/m3;arbitrary:m/s",error,sizeof(error)));
    strcpy(unit,"sentinel");
    CHECK(cfg_unit_for_key(&c,"pm25",unit,sizeof(unit)) && !strcmp(unit,"ug/m3"));
    strcpy(unit,"sentinel");
    CHECK(!cfg_unit_for_key(&c,"absent",unit,sizeof(unit)) && !strcmp(unit,"sentinel"));
    CHECK(!cfg_unit_for_key(&c,"pm25",unit,3) && !strcmp(unit,"sentinel"));
    (void)snprintf(path,sizeof(path),"tb-mav-config-%u.ini",process_id());
    CHECK(cfg_save(path,&c,4,error,sizeof(error)));
    CHECK(cfg_load(path,&loaded,4,error,sizeof(error)));
    CHECK(!strcmp(loaded.input_mode,c.input_mode) && !strcmp(loaded.serial_port,c.serial_port));
    CHECK(!strcmp(loaded.websocket_url,c.websocket_url) && !strcmp(loaded.unit_map,c.unit_map));
    CHECK(loaded.serial_baud==c.serial_baud && loaded.auto_scan==c.auto_scan);
    CHECK(loaded.probe_ms==c.probe_ms && loaded.link_timeout_ms==c.link_timeout_ms && loaded.reconnect_ms==c.reconnect_ms);
    CHECK(loaded.target_system==c.target_system && loaded.target_component==c.target_component);
    CHECK(loaded.system_time_uncert_us==c.system_time_uncert_us && loaded.duration_ms==c.duration_ms);
    CHECK(loaded.accel_bound_mps2==c.accel_bound_mps2);
    CHECK(loaded.origin_auto==c.origin_auto);
    {
        const char duplicate[]="url=-\nstream=s\nsession=x\nunit_map=a:u\nunit_map=b:u\n";
        CHECK(invalid_file(path,duplicate,sizeof(duplicate)-1,&c));
    }
    CHECK(remove_utf8(path));
    return 1;
}

/* Both input and output endpoints use the same strict literal/port/bool rules. */
static int check_udp_fields(tb_config *c, const char *host_key,
                            const char *port_key, const char *enabled_key)
{
    char error[160];
    const char *valid[] = {"127.0.0.1", "0.0.0.0", "255.255.255.255", "192.168.10.1", "10.0.0.2"};
    const char *invalid[] = {"", "localhost", "::1", "udp://127.0.0.1", "127.0.0", "127.0.0.1.2",
        "256.0.0.1", "1.2.3.999", "-1.2.3.4", "01.2.3.4", "1..3.4", "1.2.3.", "1.2.3.4:14551"};
    size_t i;
    for(i=0;i<sizeof(valid)/sizeof(valid[0]);++i)
        CHECK(cfg_set(c,host_key,valid[i],error,sizeof(error)));
    for(i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) CHECK(invalid_set(c,host_key,invalid[i]));
    CHECK(cfg_set(c,port_key,"1",error,sizeof(error)));
    CHECK(cfg_set(c,port_key,"65535",error,sizeof(error)));
    CHECK(invalid_set(c,port_key,"0"));
    CHECK(invalid_set(c,port_key,"65536"));
    CHECK(invalid_set(c,port_key,"-1"));
    CHECK(invalid_set(c,port_key,"14551.0"));
    CHECK(cfg_set(c,enabled_key,"0",error,sizeof(error)));
    CHECK(cfg_set(c,enabled_key,"1",error,sizeof(error)));
    CHECK(invalid_set(c,enabled_key,"2"));
    CHECK(invalid_set(c,enabled_key,"true"));
    return 1;
}

static int check_udp_settings(void)
{
    tb_config c, loaded, saved;
    char error[160], path[128];
    cfg_defaults(&c);
    CHECK(!strcmp(c.udp_bind,"127.0.0.1") && c.udp_port==14540 && c.udp_enabled==1);
    CHECK(valid_config(&c));
    CHECK(check_udp_fields(&c,"udp_bind","udp_port","udp_enabled"));
    CHECK(!strcmp(c.udp_bind,"10.0.0.2") && c.udp_port==65535 && c.udp_enabled==1);
    saved=c; memset(c.udp_bind,'x',sizeof(c.udp_bind));
    CHECK(!cfg_validate(&c,4,error,sizeof(error))); c=saved;
    CHECK(cfg_set(&c,"udp_bind","0.0.0.0",error,sizeof(error)));
    CHECK(cfg_set(&c,"udp_enabled","0",error,sizeof(error)));
    (void)snprintf(path,sizeof(path),"tb-udp-config-%u.ini",process_id());
    CHECK(cfg_save(path,&c,4,error,sizeof(error)));
    CHECK(cfg_load(path,&loaded,4,error,sizeof(error)));
    CHECK(!strcmp(loaded.udp_bind,"0.0.0.0") && loaded.udp_port==65535 && loaded.udp_enabled==0);
    {
        const char duplicate[]="url=-\nstream=s\nsession=x\nudp_enabled=0\nudp_enabled=1\n";
        CHECK(invalid_file(path,duplicate,sizeof(duplicate)-1,&c));
    }
    CHECK(remove_utf8(path));
    return 1;
}

static int check_qgc_settings(void)
{
    tb_config c, loaded, saved;
    char error[160], path[128], duplicate[128];
    const char *keys[] = {"qgc_host", "qgc_port", "qgc_enabled"};
    const char *values[] = {"127.0.0.1", "14550", "1"};
    const char legacy[] = "url=-\nstream=s\nsession=legacy\n";
    size_t i;
    cfg_defaults(&c);
    CHECK(!strcmp(c.qgc_host,"127.0.0.1") && c.qgc_port==14550 && c.qgc_enabled==1);
    CHECK(valid_config(&c));
    CHECK(check_udp_fields(&c,"qgc_host","qgc_port","qgc_enabled"));
    CHECK(!strcmp(c.qgc_host,"10.0.0.2") && c.qgc_port==65535 && c.qgc_enabled==1);
    /* Output edits must never alter the independent input listener. */
    CHECK(!strcmp(c.udp_bind,"127.0.0.1") && c.udp_port==14540 && c.udp_enabled==1);
    saved=c; memset(c.qgc_host,'x',sizeof(c.qgc_host));
    CHECK(!cfg_validate(&c,4,error,sizeof(error))); c=saved;
    c.qgc_port=0;
    CHECK(!cfg_validate(&c,4,error,sizeof(error))); c=saved;
    c.qgc_enabled=2;
    CHECK(!cfg_validate(&c,4,error,sizeof(error))); c=saved;
    (void)snprintf(path,sizeof(path),"tb-qgc-config-%u.ini",process_id());
    for(i=0;i<sizeof(keys)/sizeof(keys[0]);++i) {
        (void)snprintf(duplicate,sizeof(duplicate),"url=-\nstream=s\nsession=x\n%s=%s\n%s=%s\n",
                       keys[i],values[i],keys[i],values[i]);
        CHECK(invalid_file(path,duplicate,strlen(duplicate),&c));
    }
    CHECK(write_bytes(path,legacy,sizeof(legacy)-1));
    CHECK(cfg_load(path,&loaded,4,error,sizeof(error)));
    CHECK(!strcmp(loaded.qgc_host,"127.0.0.1") && loaded.qgc_port==14550 && loaded.qgc_enabled==1);
    CHECK(cfg_set(&c,"qgc_enabled","0",error,sizeof(error)) && c.qgc_enabled==0);
    CHECK(cfg_save(path,&c,4,error,sizeof(error)));
    CHECK(cfg_load(path,&loaded,4,error,sizeof(error)));
    CHECK(!strcmp(loaded.qgc_host,c.qgc_host) && loaded.qgc_port==c.qgc_port && loaded.qgc_enabled==0);
    CHECK(!strcmp(loaded.udp_bind,c.udp_bind) && loaded.udp_port==c.udp_port && loaded.udp_enabled==c.udp_enabled);
    CHECK(remove_utf8(path));
    return 1;
}

/* Distances and gap limits are independent of the 1-second send interval. */
static int check_spatial_settings(void)
{
    tb_config c, loaded, saved;
    char error[160], path[128];
    static const char *bad_step[] = {"0", "0.009999", "10000.001", "-1", "nan", "inf", "1e309", "1m"};
    static const char *bad_gap[] = {"0", "99", "60001", "-1", "2000.0", "4294967296"};
    static const char legacy[] = "url=-\nstream=s\nsession=legacy\n";
    static const char duplicate[] = "url=-\nstream=s\nsession=x\nspatial_enabled=0\nspatial_enabled=1\n";
    static const char duplicate_adaptive[] = "url=-\nstream=s\nsession=x\nspatial_adaptive=0\nspatial_adaptive=1\n";
    size_t i;
    cfg_defaults(&c);
    CHECK(c.spatial_step_m==10.0 && c.spatial_gap_ms==2000 && c.spatial_enabled==1 && c.spatial_adaptive==1);
    CHECK(valid_config(&c));
    CHECK(cfg_set(&c,"spatial_step_m","0.01",error,sizeof(error)) && c.spatial_step_m==0.01);
    CHECK(cfg_set(&c,"spatial_step_m","10000",error,sizeof(error)) && c.spatial_step_m==10000.0);
    for(i=0;i<sizeof(bad_step)/sizeof(bad_step[0]);++i)
        CHECK(invalid_set(&c,"spatial_step_m",bad_step[i]));
    CHECK(cfg_set(&c,"spatial_gap_ms","100",error,sizeof(error)) && c.spatial_gap_ms==100);
    CHECK(cfg_set(&c,"spatial_gap_ms","60000",error,sizeof(error)) && c.spatial_gap_ms==60000);
    for(i=0;i<sizeof(bad_gap)/sizeof(bad_gap[0]);++i)
        CHECK(invalid_set(&c,"spatial_gap_ms",bad_gap[i]));
    CHECK(cfg_set(&c,"spatial_enabled","0",error,sizeof(error)) && c.spatial_enabled==0);
    CHECK(cfg_set(&c,"spatial_enabled","1",error,sizeof(error)) && c.spatial_enabled==1);
    CHECK(invalid_set(&c,"spatial_enabled","2"));
    CHECK(invalid_set(&c,"spatial_enabled","true"));
    CHECK(cfg_set(&c,"spatial_adaptive","0",error,sizeof(error)) && c.spatial_adaptive==0);
    CHECK(cfg_set(&c,"spatial_adaptive","1",error,sizeof(error)) && c.spatial_adaptive==1);
    CHECK(invalid_set(&c,"spatial_adaptive","2"));
    CHECK(invalid_set(&c,"spatial_adaptive","true"));
    saved=c; c.spatial_step_m=NAN;
    CHECK(!cfg_validate(&c,4,error,sizeof(error))); c=saved;
    c.spatial_step_m=0.009;
    CHECK(!cfg_validate(&c,4,error,sizeof(error))); c=saved;
    c.spatial_gap_ms=99;
    CHECK(!cfg_validate(&c,4,error,sizeof(error))); c=saved;
    c.spatial_gap_ms=60001;
    CHECK(!cfg_validate(&c,4,error,sizeof(error))); c=saved;
    c.spatial_enabled=2;
    CHECK(!cfg_validate(&c,4,error,sizeof(error))); c=saved;
    c.spatial_adaptive=2;
    CHECK(!cfg_validate(&c,4,error,sizeof(error))); c=saved;
    (void)snprintf(path,sizeof(path),"tb-spatial-config-%u.ini",process_id());
    CHECK(invalid_file(path,duplicate,sizeof(duplicate)-1,&c));
    CHECK(invalid_file(path,duplicate_adaptive,sizeof(duplicate_adaptive)-1,&c));
    CHECK(write_bytes(path,legacy,sizeof(legacy)-1));
    CHECK(cfg_load(path,&loaded,4,error,sizeof(error)));
    CHECK(loaded.spatial_step_m==10.0 && loaded.spatial_gap_ms==2000 && loaded.spatial_enabled==1 && loaded.spatial_adaptive==1);
    CHECK(cfg_set(&c,"spatial_step_m","2.5",error,sizeof(error)));
    CHECK(cfg_set(&c,"spatial_gap_ms","1234",error,sizeof(error)));
    CHECK(cfg_set(&c,"spatial_enabled","0",error,sizeof(error)));
    CHECK(cfg_set(&c,"spatial_adaptive","0",error,sizeof(error)));
    CHECK(cfg_save(path,&c,4,error,sizeof(error)));
    CHECK(cfg_load(path,&loaded,4,error,sizeof(error)));
    CHECK(loaded.spatial_step_m==2.5 && loaded.spatial_gap_ms==1234 && loaded.spatial_enabled==0 && loaded.spatial_adaptive==0);
    CHECK(loaded.send_interval_ms==1000);
    CHECK(!memcmp(&loaded,&c,sizeof(c)));
    CHECK(remove_utf8(path));
    return 1;
}

static int check_status_settings(void)
{
    tb_config c, loaded, saved;
    char error[160], path[128];
    static const char *valid[] = {"0", "1000", "10000", "60000"};
    static const char *invalid[] = {"1", "999", "60001", "-1", "1000.5", "4294967296"};
    static const char duplicate[] = "url=-\nstream=s\nsession=x\nstatus_interval_ms=0\nstatus_interval_ms=10000\n";
    static const char legacy[] = "url=-\nstream=s\nsession=legacy\n";
    size_t i;
    cfg_defaults(&c);
    CHECK(c.status_interval_ms==0 && c.mavlink_rates_enabled==0);
    CHECK(valid_config(&c));
    for(i=0;i<sizeof(valid)/sizeof(valid[0]);++i)
        CHECK(cfg_set(&c,"status_interval_ms",valid[i],error,sizeof(error)));
    for(i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i)
        CHECK(invalid_set(&c,"status_interval_ms",invalid[i]));
    saved=c; c.status_interval_ms=999;
    CHECK(!cfg_validate(&c,4,error,sizeof(error))); c=saved;
    c.status_interval_ms=60001;
    CHECK(!cfg_validate(&c,4,error,sizeof(error))); c=saved;
    (void)snprintf(path,sizeof(path),"tb-status-config-%u.ini",process_id());
    CHECK(invalid_file(path,duplicate,sizeof(duplicate)-1,&c));
    CHECK(write_bytes(path,legacy,sizeof(legacy)-1));
    CHECK(cfg_load(path,&loaded,4,error,sizeof(error)) && loaded.status_interval_ms==0 && loaded.mavlink_rates_enabled==0);
    CHECK(invalid_set(&c,"mavlink_rates_enabled","2"));
    CHECK(invalid_set(&c,"mavlink_rates_enabled","-1"));
    CHECK(cfg_set(&c,"mavlink_rates_enabled","1",error,sizeof(error)));
    CHECK(cfg_set(&c,"status_interval_ms","10000",error,sizeof(error)));
    CHECK(cfg_save(path,&c,4,error,sizeof(error)));
    CHECK(cfg_load(path,&loaded,4,error,sizeof(error)));
    CHECK(loaded.status_interval_ms==10000 && loaded.mavlink_rates_enabled==1 && !memcmp(&loaded,&c,sizeof(c)));
    CHECK(remove_utf8(path));
    return 1;
}

static int check_udp_backup_settings(void)
{
    static const struct { const char *url,*host; unsigned port; } derived[]={
        {"http://127.0.0.1:21000/ingest","127.0.0.1",21001},
        {"http://localhost/ingest","localhost",81},
        {"https://sensor.example/ingest?port=9000","sensor.example",444},
        {"https://sensor.example:65534/a","sensor.example",65535},
        {"http://[::1]:21000/ingest","::1",21001},
        {"https://[2001:db8::2]/ingest","2001:db8::2",444},
        {"http://[::ffff:192.0.2.1]:9000/","::ffff:192.0.2.1",9001}
    };
    static const char *hosts[]={"auto","localhost","telemetry.example.","192.0.2.1",
        "::","::1","2001:db8::1","1:2:3:4:5:6:7:8","::ffff:192.0.2.1","[2001:db8::1]"};
    static const char *bad_hosts[]={"","udp://localhost","user:PRIVATE_PASSWORD@host","host/path",
        "host?token=PRIVATE","host:21001","host\\path","host\r\nINJECT","host name",
        "127.1","01.2.3.4","256.0.0.1","-host.example","host-.example","host..example",
        "[127.0.0.1]","[::1","[::1]:1234",":",":::1","1::2::3","1:2:3:4:5:6:7",
        "1:2:3:4:5:6:7:8:9","1:2:3:4:5:6:7::8","12345::1","::ffff:192.0.2.999","fe80::1%eth0"};
    tb_config c,saved,loaded;
    char host[256],error[160],path[128],duplicate[256];
    unsigned port;
    size_t i;
    CHECK(valid_config(&c));
    CHECK(!c.udp_backup_enabled && !strcmp(c.udp_backup_host,"auto") && !c.udp_backup_port);
    CHECK(cfg_set(&c,"udp_backup_enabled","1",error,sizeof(error)));
    CHECK(invalid_set(&c,"udp_backup_enabled","2"));
    CHECK(invalid_set(&c,"udp_backup_enabled","true"));
    CHECK(invalid_set(&c,"udp_backup_port","65536"));
    CHECK(invalid_set(&c,"udp_backup_port","-1"));
    for(i=0;i<sizeof(derived)/sizeof(derived[0]);++i) {
        CHECK(cfg_set(&c,"url",derived[i].url,error,sizeof(error)));
        CHECK(cfg_validate(&c,4,error,sizeof(error)));
        CHECK(cfg_udp_backup_destination(&c,host,sizeof(host),&port,error,sizeof(error)));
        CHECK(!strcmp(host,derived[i].host) && port==derived[i].port);
    }
    for(i=0;i<sizeof(hosts)/sizeof(hosts[0]);++i)
        CHECK(cfg_set(&c,"udp_backup_host",hosts[i],error,sizeof(error)));
    for(i=0;i<sizeof(bad_hosts)/sizeof(bad_hosts[0]);++i)
        CHECK(invalid_set(&c,"udp_backup_host",bad_hosts[i]));
    CHECK(invalid_set(&c,"url","http://[:::1]/ingest"));
    CHECK(invalid_set(&c,"url","http://[1:2:3:4:5:6:7]/ingest"));
    CHECK(cfg_udp_backup_destination(&c,host,sizeof(host),&port,error,sizeof(error)));
    CHECK(!strcmp(host,"2001:db8::1") && port==9001); /* Independent host override. */
    CHECK(cfg_set(&c,"udp_backup_port","65000",error,sizeof(error)));
    CHECK(cfg_set(&c,"udp_backup_host","auto",error,sizeof(error)));
    CHECK(cfg_udp_backup_destination(&c,host,sizeof(host),&port,error,sizeof(error)));
    CHECK(!strcmp(host,"::ffff:192.0.2.1") && port==65000); /* Independent port override. */

    CHECK(cfg_set(&c,"url","http://example.com:65535/ingest",error,sizeof(error)));
    CHECK(cfg_validate(&c,4,error,sizeof(error))); /* Explicit UDP port avoids overflow. */
    CHECK(cfg_set(&c,"udp_backup_port","0",error,sizeof(error)));
    strcpy(host,"sentinel"); port=17;
    CHECK(!cfg_udp_backup_destination(&c,host,sizeof(host),&port,error,sizeof(error)));
    CHECK(!host[0] && !port && error[0]);
    CHECK(!cfg_validate(&c,4,error,sizeof(error)));
    CHECK(cfg_set(&c,"udp_backup_enabled","0",error,sizeof(error)));
    CHECK(cfg_validate(&c,4,error,sizeof(error)));
    CHECK(cfg_set(&c,"url","-",error,sizeof(error)));
    CHECK(cfg_validate(&c,4,error,sizeof(error)));
    CHECK(cfg_set(&c,"udp_backup_enabled","1",error,sizeof(error)));
    CHECK(!cfg_validate(&c,4,error,sizeof(error)));
    CHECK(cfg_set(&c,"udp_backup_port","21001",error,sizeof(error)));
    CHECK(!cfg_validate(&c,4,error,sizeof(error))); /* Host remains automatic. */
    CHECK(cfg_set(&c,"udp_backup_host","::1",error,sizeof(error)));
    CHECK(cfg_validate(&c,4,error,sizeof(error)));
    CHECK(cfg_udp_backup_destination(&c,host,sizeof(host),&port,error,sizeof(error)));
    CHECK(!strcmp(host,"::1") && port==21001);
    CHECK(cfg_set(&c,"udp_backup_port","0",error,sizeof(error)));
    CHECK(!cfg_validate(&c,4,error,sizeof(error))); /* Port remains automatic. */
    CHECK(cfg_set(&c,"udp_backup_port","1",error,sizeof(error)));
    saved=c;
    CHECK(!cfg_udp_backup_destination(NULL,host,sizeof(host),&port,error,sizeof(error)));
    CHECK(!host[0] && !port);
    CHECK(!cfg_udp_backup_destination(&c,host,2,&port,error,sizeof(error)));
    CHECK(!host[0] && !port);
    memset(c.udp_backup_host,'x',sizeof(c.udp_backup_host));
    CHECK(!cfg_udp_backup_destination(&c,host,sizeof(host),&port,error,sizeof(error))); c=saved;
    memset(c.url,'x',sizeof(c.url));
    CHECK(!cfg_udp_backup_destination(&c,host,sizeof(host),&port,error,sizeof(error))); c=saved;
    strcpy(c.url,"http://PRIVATE_USER:PRIVATE_PASSWORD@example.com/path");
    CHECK(!cfg_udp_backup_destination(&c,host,sizeof(host),&port,error,sizeof(error)));
    CHECK(strstr(error,"PRIVATE")==NULL); c=saved;

    (void)snprintf(path,sizeof(path),"tb-backup-config-%u.ini",process_id());
    CHECK(cfg_save(path,&c,4,error,sizeof(error)));
    CHECK(cfg_load(path,&loaded,4,error,sizeof(error)));
    CHECK(loaded.udp_backup_enabled==c.udp_backup_enabled && loaded.udp_backup_port==c.udp_backup_port);
    CHECK(!strcmp(loaded.udp_backup_host,c.udp_backup_host) && !strcmp(loaded.url,c.url));
    for(i=42;i<45;++i) {
        const char *key=cfg_field_name((unsigned)i);
        const char *value=i==43 ? "auto" : "0";
        (void)snprintf(duplicate,sizeof(duplicate),"url=-\nstream=s\nsession=x\n%s=%s\n%s=%s\n",
                       key,value,key,value);
        CHECK(invalid_file(path,duplicate,strlen(duplicate),&c));
    }
    CHECK(remove_utf8(path));
    return 1;
}

static int check_direct_token(void)
{
    static const char credential[]="TEST_ONLY_direct.token-+/=_~";
    static const char environment_name[]="MTR_TEST_DIRECT_TOKEN";
    static const char manual[]="url=-\nstream=s\nsession=t\ntoken=TEST_ONLY_manual\n";
    static const char *reserved[]={"dpapi:TEST_ONLY_literal","plain:TEST_ONLY_literal"};
    tb_config c,loaded;
    char error[192],path[128],text[8192],value[CFG_AUTH_TOKEN_BYTES+1];
    char malformed[256];
    FILE *file;
    size_t n,i;
    unsigned index=0;
    while(index<cfg_field_count() && strcmp(cfg_field_name(index),"token")) ++index;
    CHECK(valid_config(&c));
    CHECK(index<cfg_field_count() && cfg_field_is_secret(index));
    CHECK(!cfg_field_is_secret(0) && cfg_auth_token(NULL)==NULL);
    CHECK(cfg_set(&c,"token_env",environment_name,error,sizeof(error)));
#ifdef _WIN32
    CHECK(_putenv_s(environment_name,"TEST_ONLY_environment")==0);
#else
    CHECK(setenv(environment_name,"TEST_ONLY_environment",1)==0);
#endif
    CHECK(!strcmp(cfg_auth_token(&c),"TEST_ONLY_environment"));
    CHECK(cfg_set(&c,"token",credential,error,sizeof(error)));
    CHECK(!strcmp(cfg_auth_token(&c),credential));
    CHECK(cfg_field_value(&c,index,value,sizeof(value),0) && !strcmp(value,"********"));
    CHECK(cfg_field_value(&c,index,value,sizeof(value),1) && !strcmp(value,credential));
    CHECK(invalid_set(&c,"token","TEST_ONLY space"));
    CHECK(invalid_set(&c,"token"," TEST_ONLY"));
    CHECK(invalid_set(&c,"token","TEST_ONLY "));
    CHECK(invalid_set(&c,"token","TEST_ONLY\r\nInjected:1"));
    CHECK(invalid_set(&c,"token","TEST_ONLY\tvalue"));
    CHECK(invalid_set(&c,"token","TEST_ONLY\x7f"));
    CHECK(invalid_set(&c,"token","TEST_ONLY\x80"));
    memset(value,'x',CFG_AUTH_TOKEN_MAX+1); value[CFG_AUTH_TOKEN_MAX+1]='\0';
    CHECK(invalid_set(&c,"token",value));
    value[CFG_AUTH_TOKEN_MAX]='\0';
    CHECK(cfg_set(&c,"token",value,error,sizeof(error)));
    (void)snprintf(path,sizeof(path),"test_direct_token_%u.ini",process_id());
    CHECK(cfg_save(path,&c,4,error,sizeof(error)));
    CHECK(cfg_load(path,&loaded,4,error,sizeof(error)));
    CHECK(!memcmp(&loaded,&c,sizeof(c))); /* Maximum-length encrypted roundtrip. */
    CHECK(cfg_set(&c,"token",credential,error,sizeof(error)));
    CHECK(cfg_save(path,&c,4,error,sizeof(error)));
    file=open_utf8(path,"rb"); CHECK(file!=NULL);
    n=fread(text,1,sizeof(text)-1,file); text[n]='\0';
    CHECK(!ferror(file) && fclose(file)==0);
#ifdef _WIN32
    CHECK(strstr(text,"token=dpapi:")!=NULL);
    CHECK(strstr(text,credential)==NULL);
    {
        char *cipher=strstr(text,"token=dpapi:")+strlen("token=dpapi:");
        CHECK(cipher[0]); cipher[0]=cipher[0]=='0' ? '1' : '0';
        CHECK(invalid_file(path,text,strlen(text),&c));
    }
#else
    {
        struct stat metadata;
        CHECK(strstr(text,credential)!=NULL);
        CHECK(stat(path,&metadata)==0 && (metadata.st_mode&0777)==0600);
    }
#endif
    for(i=0;i<sizeof(reserved)/sizeof(reserved[0]);++i) {
        CHECK(cfg_set(&c,"token",reserved[i],error,sizeof(error)));
        CHECK(cfg_save(path,&c,4,error,sizeof(error)));
        CHECK(cfg_load(path,&loaded,4,error,sizeof(error)));
        CHECK(!strcmp(loaded.token,reserved[i]));
    }
    CHECK(write_bytes(path,manual,sizeof(manual)-1));
    CHECK(cfg_load(path,&loaded,4,error,sizeof(error)));
    CHECK(!strcmp(loaded.token,"TEST_ONLY_manual"));
    (void)snprintf(malformed,sizeof(malformed),"url=-\nstream=s\nsession=t\ntoken=dpapi:PRIVATE_BAD_HEX\n");
    CHECK(invalid_file(path,malformed,strlen(malformed),&c));
    (void)snprintf(malformed,sizeof(malformed),"url=-\nstream=s\nsession=t\ntoken=dpapi:00\n");
    CHECK(invalid_file(path,malformed,strlen(malformed),&c));
    (void)snprintf(malformed,sizeof(malformed),"url=-\nstream=s\nsession=t\ntoken=dpapi:\n");
    CHECK(invalid_file(path,malformed,strlen(malformed),&c));
    CHECK(cfg_set(&c,"token","",error,sizeof(error)));
    CHECK(!strcmp(cfg_auth_token(&c),"TEST_ONLY_environment"));
    CHECK(cfg_field_value(&c,index,value,sizeof(value),0) && !value[0]);
    CHECK(cfg_save(path,&c,4,error,sizeof(error)));
    CHECK(cfg_load(path,&loaded,4,error,sizeof(error)) && !loaded.token[0]);
#ifdef _WIN32
    CHECK(_putenv_s(environment_name,"")==0);
#else
    CHECK(unsetenv(environment_name)==0);
#endif
    CHECK(cfg_auth_token(&c)==NULL || !cfg_auth_token(&c)[0]);
    CHECK(remove_utf8(path));
    return 1;
}

int main(void)
{
    test_workdir directory;
    int ok;
    if(!test_workdir_enter(&directory)) return 1;
    (void)setlocale(LC_NUMERIC, "C");
    ok=check_settings() && check_field_registry() && check_tcp_buffer_settings() && check_redaction() && check_url_drafts() && check_files() && check_mavlink_settings() && check_udp_settings() && check_qgc_settings() && check_spatial_settings() && check_status_settings() && check_udp_backup_settings() && check_direct_token();
    if(!test_workdir_leave(&directory)) ok=0;
    if(!ok) return 1;
    printf("PASS %u configuration checks\n", checks);
    return 0;
}
