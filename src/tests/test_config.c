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

int main(void)
{
    (void)setlocale(LC_NUMERIC, "C");
    if (!check_settings() || !check_redaction() || !check_files()) return 1;
    printf("PASS %u configuration checks\n", checks);
    return 0;
}
