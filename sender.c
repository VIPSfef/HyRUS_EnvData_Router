/* C99 receiver example: normalized CSV -> bounded queue -> 1 Hz HTTP batches. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "telemetry.h"
#include "time_sync.h"
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
typedef CRITICAL_SECTION mutex_t;
static void lock(mutex_t *m) { EnterCriticalSection(m); }
static void unlock(mutex_t *m) { LeaveCriticalSection(m); }
static uint64_t now_ms(void) { return GetTickCount64(); }
static void pause_ms(unsigned ms) { Sleep(ms); }
#else
#include <pthread.h>
#include <time.h>
#include <curl/curl.h>
typedef pthread_mutex_t mutex_t;
static void lock(mutex_t *m) { (void)pthread_mutex_lock(m); }
static void unlock(mutex_t *m) { (void)pthread_mutex_unlock(m); }
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

#ifndef TB_QUEUE_CAP
#define TB_QUEUE_CAP 8u
#endif
#ifndef TB_SEND_WINDOWS
#define TB_SEND_WINDOWS 4u
#endif
#if TB_QUEUE_CAP < 1 || TB_SEND_WINDOWS < 1 || TB_SEND_WINDOWS > TB_QUEUE_CAP
#error Invalid queue/send window capacities
#endif
#define QUEUE_CAP TB_QUEUE_CAP
#define SEND_WINDOWS TB_SEND_WINDOWS
/* Includes worst-width uint64, subnormal doubles, keys, units and error fields. */
#define BATCH_JSON_CAP (TB_CAPACITY * 512u + TB_MAX_KEYS * 768u + 2048u)
#define WIRE_CAP (SEND_WINDOWS * BATCH_JSON_CAP + 4096u)
typedef struct {
    uint64_t rejected, sequence_gaps, queue_windows, queue_samples, raw_samples;
} losses;
typedef struct {
    char key[TB_KEY_BYTES], unit[TB_UNIT_BYTES];
    uint64_t last_t;
    uint32_t last_seq;
} metric_state;
typedef struct {
    mutex_t mutex;
    tb_batch current, queue[QUEUE_CAP];
    unsigned head, count;
    uint64_t last_t, watermark_us, sealed_window;
    metric_state metric[TB_MAX_KEYS];
    unsigned metric_n;
    tc_clock clock;
    int have_last, have_sealed, eof, failed, ready, summary;
    losses loss;
    const char *url, *stream, *session;
    double origin[3];
} app;
static app state; /* Deliberately fixed storage; no large thread-stack objects. */
static char wire[WIRE_CAP];

static int token_ok(const char *s) {
    size_t n = 0;
    for (; *s; ++s, ++n)
        if (!isalnum((unsigned char)*s) && *s != '_' && *s != '-' && *s != '.') return 0;
    return n > 0 && n <= 64;
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
/* Caller holds mutex. Full queue drops NEW window, never overwrites in-flight data. */
static void seal(app *a) {
    if (!a->current.n) return;
    if (a->count == QUEUE_CAP) {
        ++a->loss.queue_windows;
        a->loss.queue_samples += a->current.n;
    } else {
        a->queue[(a->head + a->count) % QUEUE_CAP] = a->current;
        ++a->count;
    }
    a->sealed_window = a->current.window_us;
    a->have_sealed = 1;
    a->current.n = 0;
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
        (a->have_sealed && w <= a->sealed_window)) {
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
            a->last_t = s->t_us;
            a->have_last = 1;
            if (a->current.n > a->current.raw_n) ++a->loss.raw_samples;
        } else ++a->loss.rejected;
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
        unlock(&a->mutex);
    }
    return ok;
}

#ifdef _WIN32
typedef struct { HINTERNET session, connection; wchar_t route[2048]; DWORD flags; } http;
static int http_open(http *h, const char *url) {
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
    h->session = WinHttpOpen(L"telemetry-batch/1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h->session) return 0;
    if (!WinHttpSetTimeouts(h->session, 1000, 1000, 1000, 1000)) return 0;
    h->connection = WinHttpConnect(h->session, host, c.nPort, 0);
    return h->connection != NULL;
}
static int http_post(http *h, const char *body, size_t n) {
    DWORD status = 0, len = sizeof(status), redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    const char *token = getenv("TB_TOKEN");
    HINTERNET r = WinHttpOpenRequest(h->connection, L"POST", h->route, NULL,
                          WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, h->flags);
    int ok = 0;
    if (!r) return 0;
    if (!WinHttpSetOption(r, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects)) ||
        !WinHttpAddRequestHeaders(r, L"Content-Type: application/json\r\n", (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD)) goto done;
    if (token && *token) {
        char header[1152];
        wchar_t wheader[1152];
        if (strlen(token) > 1024 || strpbrk(token, "\r\n")) goto done;
        (void)snprintf(header, sizeof(header), "Authorization: Bearer %s\r\n", token);
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, header, -1, wheader, 1152) ||
            !WinHttpAddRequestHeaders(r, wheader, (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD)) goto done;
    }
    ok = WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, (void *)body,
                           (DWORD)n, (DWORD)n, 0) &&
         WinHttpReceiveResponse(r, NULL) &&
         WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX) &&
         status >= 200 && status < 300;
done:
    WinHttpCloseHandle(r);
    return ok;
}
static void http_close(http *h) {
    if (h->connection) WinHttpCloseHandle(h->connection);
    if (h->session) WinHttpCloseHandle(h->session);
}
#else
typedef struct { CURL *easy; struct curl_slist *headers; int global_ready; } http;
static size_t discard(char *p, size_t size, size_t n, void *u) {
    (void)p; (void)u; return size * n;
}
static int http_open(http *h, const char *url) {
    const char *token = getenv("TB_TOKEN");
    memset(h, 0, sizeof(*h));
    if (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) return 0;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return 0;
    h->global_ready = 1;
    h->easy = curl_easy_init();
    if (!h->easy) return 0;
    h->headers = curl_slist_append(NULL, "Content-Type: application/json");
    if (!h->headers) return 0;
    if (token && *token) {
        char header[1152];
        struct curl_slist *list;
        if (strlen(token) > 1024 || strpbrk(token, "\r\n")) return 0;
        (void)snprintf(header, sizeof(header), "Authorization: Bearer %s", token);
        list = curl_slist_append(h->headers, header);
        if (!list) return 0;
        h->headers = list;
    }
    curl_easy_setopt(h->easy, CURLOPT_URL, url);
    curl_easy_setopt(h->easy, CURLOPT_HTTPHEADER, h->headers);
    curl_easy_setopt(h->easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h->easy, CURLOPT_CONNECTTIMEOUT_MS, 1000L);
    curl_easy_setopt(h->easy, CURLOPT_TIMEOUT_MS, 3000L);
    curl_easy_setopt(h->easy, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(h->easy, CURLOPT_WRITEFUNCTION, discard);
    return 1;
}
static int http_post(http *h, const char *body, size_t n) {
    long status = 0;
    curl_easy_setopt(h->easy, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(h->easy, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)n);
    if (curl_easy_perform(h->easy) != CURLE_OK) return 0;
    return curl_easy_getinfo(h->easy, CURLINFO_RESPONSE_CODE, &status) == CURLE_OK && status >= 200 && status < 300;
}
static void http_close(http *h) {
    if (h->easy) curl_easy_cleanup(h->easy);
    curl_slist_free_all(h->headers);
    if (h->global_ready) curl_global_cleanup();
}
#endif

static int encode(app *a, unsigned head, unsigned n, losses l, const tc_report *report) {
    unsigned i;
    int k;
    size_t used;
    k = snprintf(wire, sizeof(wire),
        "{\"schema\":2,\"stream\":\"%s\",\"session\":\"%s\",\"frame\":\"ENU\","
        "\"origin_wgs84\":[%.17g,%.17g,%.17g],\"time_basis\":\"pixhawk_boot_us\","
        "\"time_epoch\":\"session_boot\",\"utc_basis\":\"unix_us_estimate\","
        "\"field_order\":[\"t_us\",\"seq\",\"quality\",\"key\",\"unit\",\"east_m\",\"north_m\",\"up_m\","
        "\"ve_mps\",\"vn_mps\",\"vu_mps\",\"value\",\"time_uncert_us\",\"nav_age_us\",\"pos_error_m\",\"utc_us\",\"utc_uncert_us\"],"
        "\"loss\":{\"rejected\":\"%" PRIu64 "\",\"sequence_gaps\":\"%" PRIu64 "\","
        "\"queue_windows\":\"%" PRIu64 "\",\"queue_samples\":\"%" PRIu64 "\",\"raw_samples\":\"%" PRIu64 "\"},\"clock_model\":",
        a->stream, a->session, a->origin[0], a->origin[1], a->origin[2],
        l.rejected, l.sequence_gaps, l.queue_windows, l.queue_samples, l.raw_samples);
    if (k < 0 || (size_t)k >= sizeof(wire)) return 0;
    used = (size_t)k;
    if (report) {
        k = snprintf(wire + used, sizeof(wire) - used,
            "{\"anchors\":%u,\"oldest_boot_us\":\"%" PRIu64 "\",\"last_boot_us\":\"%" PRIu64 "\","
            "\"last_utc_us\":\"%" PRIu64 "\",\"offset_us\":\"%" PRId64 "\","
            "\"drift_ppm\":%.17g,\"residual_std_us\":%.17g,\"max_residual_us\":%.17g,"
            "\"anchor_uncert_max_us\":%u,\"rtt_mean_us\":%.17g,\"rtt_max_us\":%u},\"batches\":[",
            (unsigned)report->count, report->oldest_boot_us, report->last_boot_us,
            report->last_utc_us, report->offset_us, report->drift_ppm,
            report->residual_std_us, report->max_abs_residual_us,
            (unsigned)report->anchor_uncert_max_us, report->roundtrip_mean_us,
            (unsigned)report->roundtrip_max_us);
    } else k = snprintf(wire + used, sizeof(wire) - used, "null,\"batches\":[");
    if (k < 0 || (size_t)k >= sizeof(wire) - used) return 0;
    used += (size_t)k;
    for (i = 0; i < n; ++i) {
        if (i) wire[used++] = ',';
        tb_batch_prepare(&a->queue[(head + i) % QUEUE_CAP]);
        k = tb_batch_json_mode(&a->queue[(head + i) % QUEUE_CAP], wire + used,
                               sizeof(wire) - used, !a->summary);
        if (k < 0) return 0;
        used += (size_t)k;
    }
    if (used + 3 >= sizeof(wire)) return 0;
    memcpy(wire + used, "]}\n", 4);
    return (int)(used + 3);
}
#ifdef _WIN32
static DWORD WINAPI sender(void *arg)
#else
static void *sender(void *arg)
#endif
{
    app *a = arg;
    http h;
    unsigned failures = 0;
    uint64_t next = now_ms() + 1000;
    int dry = strcmp(a->url, "-") == 0;
    memset(&h, 0, sizeof(h));
    if (!dry && !http_open(&h, a->url)) {
        fprintf(stderr, "HTTP setup failed\n");
        lock(&a->mutex); a->failed = 1; unlock(&a->mutex);
        http_close(&h);
        return 0;
    }
    lock(&a->mutex); a->ready = 1; unlock(&a->mutex);
    for (;;) {
        uint64_t now = now_ms();
        unsigned n, head;
        int eof, bytes, ok, have_clock;
        losses loss;
        tc_report report;
        if (now < next) { pause_ms((unsigned)(next - now > 100 ? 100 : next - now)); continue; }
        lock(&a->mutex);
        if (a->current.n && a->eof) seal(a);
        head = a->head;
        n = a->count < SEND_WINDOWS ? a->count : SEND_WINDOWS;
        eof = a->eof;
        loss = a->loss;
        have_clock = tc_get_report(&a->clock, &report);
        unlock(&a->mutex);
        if (!n && eof) break;
        bytes = n ? encode(a, head, n, loss, have_clock ? &report : NULL) : 0;
        if (n && !bytes) {
            fprintf(stderr, "JSON capacity exceeded\n");
            lock(&a->mutex); a->failed = 1; unlock(&a->mutex);
            break;
        }
        ok = !n || (dry ? (fwrite(wire, 1, (size_t)bytes, stdout) == (size_t)bytes && fflush(stdout) == 0)
                        : http_post(&h, wire, (size_t)bytes));
        if (n && ok) {
            lock(&a->mutex);
            a->head = (a->head + n) % QUEUE_CAP;
            a->count -= n;
            unlock(&a->mutex);
            failures = 0;
        } else if (n) {
            ++failures;
            fprintf(stderr, "Send failed; retaining %u window(s), attempt %u\n", n, failures);
            if (eof && failures >= 3) {
                lock(&a->mutex); a->failed = 1; unlock(&a->mutex);
                break;
            }
        }
        /* Minimum 1 s between attempt starts; do not burst after a slow request. */
        next = now_ms();
        next = next > now + 1000 ? next : now + 1000;
    }
    if (!dry) http_close(&h);
    return 0;
}
int main(int argc, char **argv) {
    app *a = &state;
    char line[1024];
    unsigned i;
    int setup_failed = 0;
#ifdef _WIN32
    HANDLE thread;
#else
    pthread_t thread;
#endif
    (void)setlocale(LC_NUMERIC, "C");
    tc_init(&a->clock);
    if (argc == 2 && strcmp(argv[1], "--sizes") == 0) {
        printf("sample=%zu batch=%zu state=%zu wire=%zu static_total=%zu\n",
               sizeof(tb_sample), sizeof(tb_batch), sizeof(state), sizeof(wire), sizeof(state) + sizeof(wire));
        return 0;
    }
    if ((argc != 7 && argc != 8) || !token_ok(argv[2]) || !token_ok(argv[3]) ||
        (argc == 8 && strcmp(argv[7], "--summary"))) {
        fprintf(stderr, "Usage: telemetry_send URL|- STREAM SESSION ORIGIN_LAT ORIGIN_LON ELLIPSOID_ALT [--summary]\n");
        return 1;
    }
    a->url = argv[1]; a->stream = argv[2]; a->session = argv[3]; a->summary = argc == 8;
    for (i = 0; i < 3; ++i) {
        char *p = argv[4 + i];
        if (!real_number(&p, &a->origin[i], '\0') || !isfinite(a->origin[i])) return 1;
    }
    if (fabs(a->origin[0]) > 90 || fabs(a->origin[1]) > 180 || fabs(a->origin[2]) > 1e7) return 1;
#ifdef _WIN32
    InitializeCriticalSection(&a->mutex);
    thread = CreateThread(NULL, 0, sender, a, 0, NULL);
    if (!thread) return 1;
#else
    if (pthread_mutex_init(&a->mutex, NULL) || pthread_create(&thread, NULL, sender, a)) return 1;
#endif
    /* Do not block on idle stdin until the sender has validated its setup. */
    for (;;) {
        int ready;
        lock(&a->mutex); ready = a->ready; setup_failed = a->failed; unlock(&a->mutex);
        if (ready || setup_failed) break;
        pause_ms(10);
    }
    if (setup_failed) goto finish;
    while (fgets(line, sizeof(line), stdin)) {
        tb_sample sample;
        char *p = line;
        int bad = 0;
        if (!strchr(line, '\n') && !feof(stdin)) {
            int c;
            while ((c = getchar()) != '\n' && c != EOF) {}
            bad = 1;
        }
        while (isspace((unsigned char)*p)) ++p;
        if (!*p || *p == '#') continue;
        if (!bad && *p == '@') {
            if (!control(a, p)) { lock(&a->mutex); ++a->loss.rejected; unlock(&a->mutex); }
        } else if (!bad && parse(p, &sample)) ingest(a, &sample);
        else { lock(&a->mutex); ++a->loss.rejected; unlock(&a->mutex); }
        lock(&a->mutex); bad = a->failed; unlock(&a->mutex);
        if (bad) break;
    }
finish:
    lock(&a->mutex);
    if (ferror(stdin)) a->failed = 1;
    a->eof = 1;
    unlock(&a->mutex);
#ifdef _WIN32
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    DeleteCriticalSection(&a->mutex);
#else
    (void)pthread_join(thread, NULL);
    (void)pthread_mutex_destroy(&a->mutex);
#endif
    fprintf(stderr, "rejected=%" PRIu64 " sequence_gaps=%" PRIu64 " raw_dropped=%" PRIu64
            " queue_windows_dropped=%" PRIu64 " queue_samples_dropped=%" PRIu64 " pending=%u\n",
            a->loss.rejected, a->loss.sequence_gaps, a->loss.raw_samples,
            a->loss.queue_windows, a->loss.queue_samples, a->count);
    return a->failed || a->loss.rejected || a->loss.sequence_gaps || a->loss.raw_samples || a->loss.queue_windows ? 2 : 0;
}
