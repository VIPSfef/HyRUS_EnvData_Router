#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#endif
#include "input_transport.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#else
#include <curl/curl.h>
#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#endif

static int fail(it_handle *h, const char *message) {
    (void)snprintf(h->error, sizeof h->error, "%s", message);
    return -1;
}
const char *it_error(const it_handle *h) { return h->error; }

#ifdef _WIN32
static int winfail(it_handle *h, const char *message, DWORD code) {
    (void)snprintf(h->error, sizeof h->error, "%s (Windows %lu)", message, (unsigned long)code);
    return -1;
}
size_t it_enumerate(char endpoints[][IT_ENDPOINT_BYTES], size_t capacity) {
    char target[1024], name[32];
    size_t count = 0;
    unsigned number;
    if (capacity > IT_MAX_CANDIDATES) capacity = IT_MAX_CANDIDATES;
    /* QueryDosDevice avoids opening ports (including ports owned by another process). */
    for (number = 1; number <= 4096 && count < capacity; ++number) {
        (void)snprintf(name, sizeof name, "COM%u", number);
        if (QueryDosDeviceA(name, target, (DWORD)sizeof target)) {
            (void)snprintf(endpoints[count++], IT_ENDPOINT_BYTES, "%s", name);
        }
    }
    return count;
}
static int serial_open(it_handle *h, const char *endpoint, unsigned baud) {
    char path[IT_ENDPOINT_BYTES + 8];
    DCB dcb;
    COMMTIMEOUTS timeout;
    HANDLE port;
    if (!strncmp(endpoint, "\\\\.\\", 4)) (void)snprintf(path, sizeof path, "%s", endpoint);
    else (void)snprintf(path, sizeof path, "\\\\.\\%s", endpoint);
    port = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (port == INVALID_HANDLE_VALUE) return winfail(h, "Cannot open serial port", GetLastError());
    h->socket = port;
    memset(&dcb, 0, sizeof dcb);
    dcb.DCBlength = sizeof dcb;
    if (!GetCommState(port, &dcb)) return winfail(h, "Cannot read serial settings", GetLastError());
    dcb.BaudRate = baud; dcb.ByteSize = 8; dcb.Parity = NOPARITY; dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE; dcb.fParity = FALSE; dcb.fOutxCtsFlow = FALSE; dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_DISABLE; dcb.fDsrSensitivity = FALSE;
    dcb.fOutX = FALSE; dcb.fInX = FALSE; dcb.fRtsControl = RTS_CONTROL_DISABLE;
    dcb.fAbortOnError = FALSE;
    memset(&timeout, 0, sizeof timeout);
    timeout.ReadIntervalTimeout = MAXDWORD;
    timeout.ReadTotalTimeoutMultiplier = MAXDWORD;
    timeout.ReadTotalTimeoutConstant = h->io_timeout_ms; /* wait only for first byte */
    timeout.WriteTotalTimeoutConstant = h->io_timeout_ms;
    (void)SetupComm(port, 65536, 4096); /* Driver hint; no observation files. */
    if (!SetCommState(port, &dcb) || !SetCommTimeouts(port, &timeout))
        return winfail(h, "Cannot configure serial port", GetLastError());
    return 1;
}
/* WinHTTP's synchronous WebSocket receive has no receive timeout. Its single
   worker owns the pending receive; the input thread only waits on an event.
   Closing the WebSocket cancels that receive before the state can be reused. */
static DWORD WINAPI receive_worker(LPVOID context) {
    it_handle *h = (it_handle *)context;
    HANDLE waiters[2];
    waiters[0] = (HANDLE)h->stop; waiters[1] = (HANDLE)h->consumed;
    for (;;) {
        DWORD n = 0, result;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
        if (WaitForMultipleObjects(2, waiters, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) break;
        result = WinHttpWebSocketReceive((HINTERNET)h->socket, h->buffer,
                                        (DWORD)sizeof h->buffer, &n, &type);
        if (result || type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE ||
            (type != WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE &&
             type != WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE)) {
            if (result) (void)winfail(h, "WebSocket receive failed", result);
            else (void)fail(h, type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE ?
                            "WebSocket peer closed" : "WebSocket text is not a binary MAVLink stream");
            (void)InterlockedExchange(&h->worker_result, -1);
            (void)SetEvent((HANDLE)h->ready);
            break;
        }
        if (!n) { (void)SetEvent((HANDLE)h->consumed); continue; }
        h->buffered = n;
        (void)InterlockedExchange(&h->worker_result, 1);
        (void)SetEvent((HANDLE)h->ready);
    }
    return 0;
}
static int websocket_open(it_handle *h, const char *endpoint, unsigned timeout_ms) {
    wchar_t url[2052], host[2048], path[2048];
    URL_COMPONENTS parts;
    HINTERNET request = NULL;
    DWORD status = 0, bytes = sizeof status;
    int secure, ok = 0;
    char httpurl[2052];
    if (!strncmp(endpoint, "wss://", 6)) {
        secure = 1; (void)snprintf(httpurl, sizeof httpurl, "https://%s", endpoint + 6);
    } else if (!strncmp(endpoint, "ws://", 5)) {
        secure = 0; (void)snprintf(httpurl, sizeof httpurl, "http://%s", endpoint + 5);
    } else return fail(h, "WebSocket endpoint must start with ws:// or wss://");
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, httpurl, -1, url, (int)(sizeof url / sizeof url[0])))
        return fail(h, "Invalid WebSocket URL encoding");
    memset(&parts, 0, sizeof parts); parts.dwStructSize = sizeof parts;
    parts.dwHostNameLength = (DWORD)-1; parts.dwUrlPathLength = (DWORD)-1;
    parts.dwExtraInfoLength = (DWORD)-1; parts.dwUserNameLength = (DWORD)-1;
    parts.dwPasswordLength = (DWORD)-1;
    if (!WinHttpCrackUrl(url, 0, 0, &parts) || !parts.dwHostNameLength ||
        parts.dwHostNameLength >= sizeof host / sizeof host[0] ||
        parts.dwUrlPathLength + parts.dwExtraInfoLength + 2 >= sizeof path / sizeof path[0] ||
        parts.dwUserNameLength || parts.dwPasswordLength)
        return fail(h, "Invalid WebSocket URL or unsupported URL credentials");
    memcpy(host, parts.lpszHostName, parts.dwHostNameLength * sizeof(wchar_t)); host[parts.dwHostNameLength] = 0;
    if (parts.dwUrlPathLength) memcpy(path, parts.lpszUrlPath, parts.dwUrlPathLength * sizeof(wchar_t));
    else { path[0] = L'/'; parts.dwUrlPathLength = 1; }
    if (parts.dwExtraInfoLength) memcpy(path + parts.dwUrlPathLength, parts.lpszExtraInfo, parts.dwExtraInfoLength * sizeof(wchar_t));
    path[parts.dwUrlPathLength + parts.dwExtraInfoLength] = 0;
    h->session = WinHttpOpen(L"mavlink_telemetry_data_router/0.5", WINHTTP_ACCESS_TYPE_NO_PROXY, NULL, NULL, 0);
    if (!h->session) return winfail(h, "Cannot create WebSocket session", GetLastError());
    (void)WinHttpSetTimeouts((HINTERNET)h->session, (int)timeout_ms, (int)timeout_ms,
                             (int)h->io_timeout_ms, (int)timeout_ms);
    h->connection = WinHttpConnect((HINTERNET)h->session, host, parts.nPort, 0);
    if (!h->connection) return winfail(h, "Cannot connect WebSocket", GetLastError());
    request = WinHttpOpenRequest((HINTERNET)h->connection, L"GET", path, NULL, NULL,
                                WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request) return winfail(h, "Cannot create WebSocket request", GetLastError());
    if (WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0) &&
        WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, NULL) &&
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &bytes, WINHTTP_NO_HEADER_INDEX) && status == 101)
        h->socket = WinHttpWebSocketCompleteUpgrade(request, 0);
    if (!h->socket) (void)winfail(h, "WebSocket upgrade rejected", GetLastError());
    (void)WinHttpCloseHandle(request);
    if (h->socket) {
        h->ready = CreateEventW(NULL, TRUE, FALSE, NULL);
        h->consumed = CreateEventW(NULL, FALSE, TRUE, NULL);
        h->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (h->ready && h->consumed && h->stop) {
            h->worker = CreateThread(NULL, 65536, receive_worker, h, 0, NULL);
            ok = h->worker != NULL;
        }
        if (!ok) (void)winfail(h, "Cannot create WebSocket receiver", GetLastError());
    }
    return ok ? 1 : -1;
}
#else
static uint64_t monotonic_ms(void) {
    struct timespec t;
    (void)clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}
size_t it_enumerate(char endpoints[][IT_ENDPOINT_BYTES], size_t capacity) {
    const char *patterns[] = {"/dev/serial/by-id/*", "/dev/ttyACM*", "/dev/ttyUSB*"};
    size_t count = 0, p;
    struct stat seen[IT_MAX_CANDIDATES];
    if (capacity > IT_MAX_CANDIDATES) capacity = IT_MAX_CANDIDATES;
    for (p = 0; p < sizeof patterns / sizeof patterns[0] && count < capacity; ++p) {
        glob_t names;
        size_t i;
        memset(&names, 0, sizeof names);
        if (!glob(patterns[p], 0, NULL, &names)) {
            for (i = 0; i < names.gl_pathc && count < capacity; ++i) {
                struct stat s;
                size_t j;
                if (strlen(names.gl_pathv[i]) >= IT_ENDPOINT_BYTES || stat(names.gl_pathv[i], &s) || !S_ISCHR(s.st_mode)) continue;
                for (j = 0; j < count; ++j) if (seen[j].st_rdev == s.st_rdev) break;
                if (j != count) continue;
                seen[count] = s;
                (void)snprintf(endpoints[count++], IT_ENDPOINT_BYTES, "%s", names.gl_pathv[i]);
            }
        }
        globfree(&names);
    }
    return count;
}
static speed_t serial_speed(unsigned baud) {
    switch (baud) {
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
#ifdef B230400
    case 230400: return B230400;
#endif
#ifdef B460800
    case 460800: return B460800;
#endif
#ifdef B921600
    case 921600: return B921600;
#endif
    default: return (speed_t)0;
    }
}
static int serial_open(it_handle *h, const char *endpoint, unsigned baud) {
    struct termios settings;
    speed_t speed = serial_speed(baud);
    if (!speed) return fail(h, "Unsupported serial baud rate");
    h->fd = open(endpoint, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (h->fd < 0) return fail(h, strerror(errno));
    if (tcgetattr(h->fd, &settings)) return fail(h, strerror(errno));
    cfmakeraw(&settings);
    settings.c_cflag |= CLOCAL | CREAD;
#ifdef CRTSCTS
    settings.c_cflag &= (tcflag_t)~CRTSCTS;
#endif
    settings.c_cc[VMIN] = 0; settings.c_cc[VTIME] = 0;
    if (cfsetispeed(&settings, speed) || cfsetospeed(&settings, speed) || tcsetattr(h->fd, TCSANOW, &settings))
        return fail(h, strerror(errno));
    return 1;
}
static int entropy(unsigned char *out, size_t length) {
    int fd = open("/dev/urandom", O_RDONLY);
    size_t used = 0;
    if (fd < 0) return 0;
    while (used < length) {
        ssize_t n = read(fd, out + used, length - used);
        if (n > 0) used += (size_t)n;
        else if (n < 0 && errno == EINTR) continue;
        else { (void)close(fd); return 0; }
    }
    (void)close(fd); return 1;
}
static void base64(const unsigned char *in, size_t n, char *out) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i, o = 0;
    for (i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        out[o++] = table[(v >> 18) & 63]; out[o++] = table[(v >> 12) & 63];
        out[o++] = i + 1 < n ? table[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? table[v & 63] : '=';
    }
    out[o] = 0;
}
static uint32_t rol(uint32_t x, unsigned n) { return (x << n) | (x >> (32 - n)); }
/* RFC6455 accepts SHA-1 only as the upgrade challenge, not as data integrity. */
static void accept_hash(const char *key, char out[29]) {
    unsigned char block[128], digest[20];
    uint32_t a0 = 0x67452301u, b0 = 0xefcdab89u, c0 = 0x98badcfeu, d0 = 0x10325476u, e0 = 0xc3d2e1f0u;
    size_t n = strlen(key), length, offset, i;
    memset(block, 0, sizeof block);
    memcpy(block, key, n); memcpy(block + n, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", 36); n += 36;
    block[n] = 0x80; length = n < 56 ? 64 : 128;
    block[length - 2] = (unsigned char)((n * 8) >> 8); block[length - 1] = (unsigned char)(n * 8);
    for (offset = 0; offset < length; offset += 64) {
        uint32_t w[80], a = a0, b = b0, c = c0, d = d0, e = e0;
        for (i = 0; i < 16; ++i) { size_t p = offset + i * 4; w[i] = (uint32_t)block[p] << 24 | (uint32_t)block[p + 1] << 16 | (uint32_t)block[p + 2] << 8 | block[p + 3]; }
        for (i = 16; i < 80; ++i) w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
        for (i = 0; i < 80; ++i) {
            uint32_t f, k, t;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999u; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdcu; }
            else { f = b ^ c ^ d; k = 0xca62c1d6u; }
            t = rol(a, 5) + f + e + k + w[i]; e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        a0 += a; b0 += b; c0 += c; d0 += d; e0 += e;
    }
    { uint32_t state[5]; state[0]=a0; state[1]=b0; state[2]=c0; state[3]=d0; state[4]=e0;
      for (i=0;i<20;++i) digest[i]=(unsigned char)(state[i/4] >> (24 - 8*(i%4))); }
    base64(digest, sizeof digest, out);
}
static int socket_wait(it_handle *h, short events, unsigned timeout_ms) {
    struct pollfd p;
    int r;
    p.fd = h->fd; p.events = events; p.revents = 0;
    r = poll(&p, 1, (int)timeout_ms);
    if (r < 0 && errno == EINTR) return 0; /* caller retains its absolute deadline */
    if (r < 0 || (r > 0 && (p.revents & (POLLERR | POLLNVAL)))) return fail(h, "Input socket failed");
    return r;
}
static int raw_recv(it_handle *h, void *out, size_t cap) {
    size_t n = 0;
    CURLcode r = curl_easy_recv((CURL *)h->socket, out, cap, &n);
    if (r == CURLE_AGAIN) return 0;
    if (r != CURLE_OK) return fail(h, curl_easy_strerror(r));
    if (!n) return fail(h, "WebSocket peer disconnected");
    return (int)n;
}
static int raw_send_all(it_handle *h, const unsigned char *data, size_t length, unsigned timeout_ms) {
    size_t sent = 0;
    uint64_t deadline = monotonic_ms() + timeout_ms;
    while (sent < length) {
        size_t n = 0;
        CURLcode r = curl_easy_send((CURL *)h->socket, data + sent, length - sent, &n);
        if (r != CURLE_OK && r != CURLE_AGAIN) return fail(h, curl_easy_strerror(r));
        sent += n;
        if (sent == length) break;
        if (monotonic_ms() >= deadline) return fail(h, "WebSocket send timeout");
        if (socket_wait(h, POLLOUT, 5) < 0) return -1;
    }
    return (int)sent;
}
static int websocket_send(it_handle *h, unsigned opcode, const unsigned char *data, size_t length, unsigned timeout_ms) {
    unsigned char frame[4110], mask[4];
    size_t head = 2, i;
    if (length > 4096) return fail(h, "Outbound WebSocket frame exceeds 4096 bytes");
    if (!entropy(mask, sizeof mask)) return fail(h, "Cannot create WebSocket mask");
    frame[0] = (unsigned char)(0x80u | opcode);
    if (length < 126) frame[1] = (unsigned char)(0x80u | length);
    else { frame[1] = 0xfe; frame[2] = (unsigned char)(length >> 8); frame[3] = (unsigned char)length; head = 4; }
    memcpy(frame + head, mask, sizeof mask); head += sizeof mask;
    for (i = 0; i < length; ++i) frame[head + i] = data[i] ^ mask[i & 3];
    if (raw_send_all(h, frame, head + length, timeout_ms) < 0) return -1;
    return (int)length;
}
static int token_is(const char *start, size_t n, const char *word) {
    size_t i;
    if (strlen(word) != n) return 0;
    for (i = 0; i < n; ++i) {
        char c = start[i]; if (c >= 'A' && c <= 'Z') c = (char)(c + ('a'-'A'));
        if (c != word[i]) return 0;
    }
    return 1;
}
static int header_token(const char *value, const char *wanted) {
    while (*value) {
        const char *end;
        while (*value == ' ' || *value == '\t' || *value == ',') ++value;
        end = value; while (*end && *end != ',' && *end != ' ' && *end != '\t') ++end;
        if (token_is(value, (size_t)(end-value), wanted)) return 1;
        value = end;
    }
    return 0;
}
static int websocket_open(it_handle *h, const char *endpoint, unsigned timeout_ms) {
    char httpurl[2052], request[4096], response[8192], key[25], expected[29];
    unsigned char random[16];
    char *host = NULL, *port = NULL, *path = NULL, *query = NULL, *user = NULL;
    CURLU *url = NULL;
    CURL *curl;
    CURLcode result;
    curl_socket_t socket;
    size_t used = 0;
    uint64_t deadline;
    int length, status = 0, upgrade = 0, connection = 0, accepted = 0, ok = 0;
    if (!strncmp(endpoint, "wss://", 6)) (void)snprintf(httpurl, sizeof httpurl, "https://%s", endpoint+6);
    else if (!strncmp(endpoint, "ws://", 5)) (void)snprintf(httpurl, sizeof httpurl, "http://%s", endpoint+5);
    else return fail(h, "WebSocket endpoint must start with ws:// or wss://");
    if (strpbrk(httpurl, "\r\n")) return fail(h, "Invalid WebSocket URL");
    url = curl_url();
    if (!url || curl_url_set(url, CURLUPART_URL, httpurl, 0) ||
        curl_url_get(url, CURLUPART_HOST, &host, 0) || curl_url_get(url, CURLUPART_PATH, &path, 0)) {
        (void)fail(h, "Invalid WebSocket URL"); goto done;
    }
    if (!curl_url_get(url, CURLUPART_USER, &user, 0)) {
        (void)fail(h, "URL credentials are unsupported; use a configured bridge without URL credentials"); goto done;
    }
    (void)curl_url_get(url, CURLUPART_PORT, &port, 0);
    (void)curl_url_get(url, CURLUPART_QUERY, &query, 0);
    if (!entropy(random, sizeof random)) { (void)fail(h, "Cannot create WebSocket key"); goto done; }
    base64(random, sizeof random, key); accept_hash(key, expected);
    length = snprintf(request, sizeof request,
        "GET %s%s%s HTTP/1.1\r\nHost: %s%s%s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n",
        path && *path ? path : "/", query ? "?" : "", query ? query : "", host, port ? ":" : "", port ? port : "", key);
    if (length < 0 || (size_t)length >= sizeof request) { (void)fail(h, "WebSocket URL too long"); goto done; }
    curl = curl_easy_init(); h->socket = curl;
    if (!curl) { (void)fail(h, "Cannot initialize WebSocket connection"); goto done; }
    (void)curl_easy_setopt(curl, CURLOPT_URL, httpurl);
    (void)curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 1L);
    (void)curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    (void)curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)timeout_ms);
    (void)curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)timeout_ms);
    /* TLS peer/hostname verification retains libcurl's secure defaults. */
    result = curl_easy_perform(curl);
    if (result != CURLE_OK) { (void)fail(h, curl_easy_strerror(result)); goto done; }
    if (curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &socket) != CURLE_OK || socket == CURL_SOCKET_BAD) {
        (void)fail(h, "WebSocket has no connected socket"); goto done;
    }
    h->fd = (int)socket;
    if (raw_send_all(h, (const unsigned char *)request, (size_t)length, timeout_ms) < 0) goto done;
    deadline = monotonic_ms() + timeout_ms;
    while (used + 1 < sizeof response) {
        int n = raw_recv(h, response + used, 1);
        if (n < 0) goto done;
        if (n > 0) {
            used += (size_t)n;
            if (used >= 4 && !memcmp(response + used - 4, "\r\n\r\n", 4)) break;
        } else {
            if (monotonic_ms() >= deadline) { (void)fail(h, "WebSocket upgrade timeout"); goto done; }
            if (socket_wait(h, POLLIN, 5) < 0) goto done;
        }
    }
    if (used < 4 || memcmp(response + used - 4, "\r\n\r\n", 4)) { (void)fail(h, "WebSocket response headers exceed 8192 bytes"); goto done; }
    response[used] = 0;
    if (sscanf(response, "HTTP/1.1 %d", &status) != 1 || status != 101) { (void)fail(h, "WebSocket upgrade requires HTTP 101"); goto done; }
    {
        char *line = strstr(response, "\r\n");
        while (line && line[2]) {
            char *end, *colon, *value, *trim;
            line += 2; end = strstr(line, "\r\n"); if (!end || end == line) break;
            *end = 0; colon = strchr(line, ':');
            if (!colon) { (void)fail(h, "Malformed WebSocket response header"); goto done; }
            value = colon + 1; while (*value == ' ' || *value == '\t') ++value;
            trim = end; while (trim > value && (trim[-1] == ' ' || trim[-1] == '\t')) *--trim = 0;
            if (token_is(line, (size_t)(colon-line), "upgrade")) upgrade = header_token(value, "websocket");
            else if (token_is(line, (size_t)(colon-line), "connection")) connection = header_token(value, "upgrade");
            else if (token_is(line, (size_t)(colon-line), "sec-websocket-accept")) accepted = !strcmp(value, expected);
            else if (token_is(line, (size_t)(colon-line), "sec-websocket-extensions")) { (void)fail(h, "Unrequested WebSocket extension"); goto done; }
            *end = '\r'; line = end;
        }
    }
    if (!upgrade || !connection || !accepted) { (void)fail(h, "WebSocket upgrade validation failed"); goto done; }
    h->header_need = 2; ok = 1;
done:
    curl_free(host); curl_free(port); curl_free(path); curl_free(query); curl_free(user);
    if (url) curl_url_cleanup(url);
    return ok ? 1 : -1;
}
/* Frame parsing is streaming: only a 14-byte header and 125-byte control payload
   are retained. Data frames never require allocation, regardless of their size. */
static int websocket_read(it_handle *h, unsigned char *out, size_t capacity) {
    uint64_t deadline = monotonic_ms() + h->io_timeout_ms;
    unsigned operations = 0;
    while (++operations <= 256) {
        int n;
        if (operations > 1 && monotonic_ms() >= deadline) return 0;
        if (!h->frame_left && !h->header_n) h->header_need = 2;
        if (!h->frame_left) {
            while (h->header_n < h->header_need) {
                n = raw_recv(h, h->header + h->header_n, h->header_need - h->header_n);
                if (n < 0) return -1;
                if (!n) {
                    if (monotonic_ms() >= deadline) return 0;
                    if (socket_wait(h, POLLIN, 5) < 0) return -1;
                    continue;
                }
                h->header_n += (size_t)n;
                if (h->header_n == 2 && h->header_need == 2) {
                    unsigned shortlen = h->header[1] & 127u;
                    if ((h->header[0] & 0x70u) || (h->header[1] & 0x80u)) return fail(h, "Invalid masked/reserved server WebSocket frame");
                    if (shortlen == 126) h->header_need = 4;
                    else if (shortlen == 127) h->header_need = 10;
                }
            }
            h->opcode = h->header[0] & 15u; h->frame_fin = h->header[0] >> 7;
            h->frame_left = h->header[1] & 127u;
            if (h->header_need > 2) {
                size_t i;
                h->frame_left = 0;
                for (i=2; i<h->header_need; ++i) h->frame_left = (h->frame_left << 8) | h->header[i];
                if ((h->header_need == 4 && h->frame_left < 126) ||
                    (h->header_need == 10 && (h->frame_left < 65536 || (h->header[2] & 0x80u))))
                    return fail(h, "Noncanonical WebSocket frame length");
            }
            h->header_n = 0; h->control_n = 0;
            if (h->opcode >= 8) {
                if (!h->frame_fin || h->frame_left > 125 ||
                    (h->opcode != 8 && h->opcode != 9 && h->opcode != 10))
                    return fail(h, "Invalid WebSocket control frame");
            } else {
                if (h->opcode == 1) return fail(h, "WebSocket text is not a binary MAVLink stream");
                if ((h->opcode == 0 && !h->fragmented) || (h->opcode == 2 && h->fragmented) ||
                    (h->opcode != 0 && h->opcode != 2)) return fail(h, "Invalid WebSocket fragment sequence");
                h->fragmented = !h->frame_fin;
            }
        }
        if (h->opcode >= 8) {
            while (h->frame_left) {
                n = raw_recv(h, h->control + h->control_n, (size_t)h->frame_left);
                if (n < 0) return -1;
                if (!n) {
                    /* Retain header state so a partial control payload resumes. */
                    if (monotonic_ms() >= deadline) return 0;
                    if (socket_wait(h, POLLIN, 5) < 0) return -1;
                    continue;
                }
                h->control_n += (size_t)n; h->frame_left -= (unsigned)n;
            }
            if (h->opcode == 8) { (void)websocket_send(h, 8, h->control, h->control_n, 1); return fail(h, "WebSocket peer closed"); }
            if (h->opcode == 9) {
                uint64_t now = monotonic_ms();
                unsigned remaining = deadline > now ? (unsigned)(deadline - now) : 1;
                if (websocket_send(h, 10, h->control, h->control_n, remaining) < 0) return -1;
            }
            continue;
        }
        if (!h->frame_left) continue;
        {
            size_t take = h->frame_left < (uint64_t)capacity ? (size_t)h->frame_left : capacity;
            n = raw_recv(h, out, take);
        }
        if (n < 0) return -1;
        if (!n) {
            if (monotonic_ms() >= deadline) return 0;
            if (socket_wait(h, POLLIN, 5) < 0) return -1;
            continue;
        }
        h->frame_left -= (unsigned)n;
        return n;
    }
    return 0;
}
#endif

int it_open(it_handle *h, it_kind kind, const char *endpoint, unsigned baud,
            unsigned connect_timeout_ms, unsigned io_timeout_ms) {
    int result;
    memset(h, 0, sizeof *h); h->fd = -1; h->kind = (int)kind;
    h->io_timeout_ms = io_timeout_ms ? io_timeout_ms : 100;
    if (h->io_timeout_ms > 100) h->io_timeout_ms = 100;
    if (!endpoint || !*endpoint || strlen(endpoint) >= (kind == IT_WEBSOCKET ? IT_WEBSOCKET_ENDPOINT_BYTES : IT_ENDPOINT_BYTES) || strpbrk(endpoint, "\r\n"))
        return fail(h, "Invalid input endpoint");
    if (kind == IT_WEBSOCKET && strchr(endpoint, '#')) return fail(h, "WebSocket URL fragments are unsupported");
    if (!connect_timeout_ms) connect_timeout_ms = 1000;
    if (connect_timeout_ms > 60000) connect_timeout_ms = 60000;
    if (kind == IT_SERIAL) result = serial_open(h, endpoint, baud);
    else if (kind == IT_WEBSOCKET) result = websocket_open(h, endpoint, connect_timeout_ms);
    else result = fail(h, "Unsupported input transport");
    if (result < 0) { it_close(h); return 0; }
    h->opened = 1; return 1;
}
int it_read(it_handle *h, void *output, size_t capacity) {
    if (!h->opened) return fail(h, "Input is not open");
    if (!capacity) return 0;
    if (capacity > INT_MAX) capacity = INT_MAX;
#ifdef _WIN32
    if (h->kind == IT_SERIAL) {
        DWORD n = 0;
        if (!ReadFile((HANDLE)h->socket, output, (DWORD)capacity, &n, NULL)) return winfail(h, "Serial read failed", GetLastError());
        return (int)n;
    }
    {
        size_t take;
        DWORD r = WaitForSingleObject((HANDLE)h->ready, h->io_timeout_ms);
        if (r == WAIT_TIMEOUT) return 0;
        if (r != WAIT_OBJECT_0) return winfail(h, "WebSocket wait failed", GetLastError());
        if (InterlockedCompareExchange(&h->worker_result, 0, 0) < 0) return -1;
        take = h->buffered - h->offset; if (take > capacity) take = capacity;
        memcpy(output, h->buffer + h->offset, take); h->offset += take;
        if (h->offset == h->buffered) {
            h->offset = 0; h->buffered = 0;
            (void)ResetEvent((HANDLE)h->ready); (void)SetEvent((HANDLE)h->consumed);
        }
        return (int)take;
    }
#else
    if (h->kind == IT_SERIAL) {
        int r = socket_wait(h, POLLIN, h->io_timeout_ms);
        ssize_t n;
        if (r <= 0) return r;
        n = read(h->fd, output, capacity);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
        if (n <= 0) return fail(h, n ? strerror(errno) : "Serial device disconnected");
        return (int)n;
    }
    return websocket_read(h, (unsigned char *)output, capacity);
#endif
}
int it_write(it_handle *h, const void *data, size_t length) {
    if (!h->opened) return fail(h, "Input is not open");
    if (!length) return 0;
    if (length > INT_MAX) return fail(h, "Input write too large");
#ifdef _WIN32
    if (h->kind == IT_SERIAL) {
        DWORD n = 0;
        if (!WriteFile((HANDLE)h->socket, data, (DWORD)length, &n, NULL)) return winfail(h, "Serial write failed", GetLastError());
        return (int)n;
    }
    if (length > 4096) return fail(h, "Outbound WebSocket frame exceeds 4096 bytes");
    {
        DWORD r = WinHttpWebSocketSend((HINTERNET)h->socket, WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE,
                                      (void *)data, (DWORD)length);
        return r ? winfail(h, "WebSocket send failed", r) : (int)length;
    }
#else
    if (h->kind == IT_SERIAL) {
        ssize_t n = write(h->fd, data, length);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
        return n < 0 ? fail(h, strerror(errno)) : (int)n;
    }
    return websocket_send(h, 2, (const unsigned char *)data, length, h->io_timeout_ms);
#endif
}
void it_close(it_handle *h) {
#ifdef _WIN32
    if (h->kind == IT_WEBSOCKET) {
        if (h->stop) (void)SetEvent((HANDLE)h->stop);
        if (h->socket) (void)WinHttpCloseHandle((HINTERNET)h->socket);
        h->socket = NULL;
        if (h->worker) { (void)WaitForSingleObject((HANDLE)h->worker, INFINITE); (void)CloseHandle((HANDLE)h->worker); }
        if (h->ready) (void)CloseHandle((HANDLE)h->ready);
        if (h->consumed) (void)CloseHandle((HANDLE)h->consumed);
        if (h->stop) (void)CloseHandle((HANDLE)h->stop);
        if (h->connection) (void)WinHttpCloseHandle((HINTERNET)h->connection);
        if (h->session) (void)WinHttpCloseHandle((HINTERNET)h->session);
    } else if (h->socket) { (void)CloseHandle((HANDLE)h->socket); h->socket = NULL; }
    h->session = h->connection = h->worker = h->ready = h->consumed = h->stop = NULL;
#else
    if (h->kind == IT_WEBSOCKET && h->socket) curl_easy_cleanup((CURL *)h->socket);
    else if (h->fd >= 0) (void)close(h->fd);
    h->socket = NULL; h->fd = -1;
#endif
    h->opened = 0;
}