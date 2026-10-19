/* Independent strict configuration parsing, boundaries and file transaction tests. */
#include "config.h"

#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <process.h>
#else
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
    CHECK(invalid_set(&c, "token", "PRIVATE_TOKEN_VALUE"));
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

static int check_udp_settings(void)
{
    tb_config c, loaded, saved;
    char error[160], path[128];
    const char *valid[] = {"127.0.0.1", "0.0.0.0", "255.255.255.255", "192.168.10.1", "10.0.0.2"};
    const char *invalid[] = {"", "localhost", "::1", "udp://127.0.0.1", "127.0.0", "127.0.0.1.2",
        "256.0.0.1", "1.2.3.999", "-1.2.3.4", "01.2.3.4", "1..3.4", "1.2.3.", "1.2.3.4:14551"};
    size_t i;
    cfg_defaults(&c);
    CHECK(!strcmp(c.udp_bind,"127.0.0.1") && c.udp_port==14551 && c.udp_enabled==1);
    CHECK(valid_config(&c));
    for(i=0;i<sizeof(valid)/sizeof(valid[0]);++i) {
        CHECK(cfg_set(&c,"udp_bind",valid[i],error,sizeof(error)));
        CHECK(!strcmp(c.udp_bind,valid[i]));
    }
    for(i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) CHECK(invalid_set(&c,"udp_bind",invalid[i]));
    CHECK(cfg_set(&c,"udp_port","1",error,sizeof(error)) && c.udp_port==1);
    CHECK(cfg_set(&c,"udp_port","65535",error,sizeof(error)) && c.udp_port==65535);
    CHECK(invalid_set(&c,"udp_port","0"));
    CHECK(invalid_set(&c,"udp_port","65536"));
    CHECK(invalid_set(&c,"udp_port","-1"));
    CHECK(invalid_set(&c,"udp_port","14551.0"));
    CHECK(cfg_set(&c,"udp_enabled","0",error,sizeof(error)) && c.udp_enabled==0);
    CHECK(cfg_set(&c,"udp_enabled","1",error,sizeof(error)) && c.udp_enabled==1);
    CHECK(invalid_set(&c,"udp_enabled","2"));
    CHECK(invalid_set(&c,"udp_enabled","true"));
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

int main(void)
{
    (void)setlocale(LC_NUMERIC, "C");
    if (!check_settings() || !check_redaction() || !check_files() || !check_mavlink_settings() || !check_udp_settings()) return 1;
    printf("PASS %u configuration checks\n", checks);
    return 0;
}
