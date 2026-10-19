/* Developer transport probe; no external server or physical serial device needed. */
#include "input_transport.h"
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <curl/curl.h>
#endif
int main(int argc, char **argv) {
    it_handle transport;
    unsigned char data[64];
    int i, n, total = 0, loops, result;
    it_kind kind;
#ifndef _WIN32
    if (curl_global_init(CURL_GLOBAL_DEFAULT)) return 2;
#endif
    if (argc == 2 && !strcmp(argv[1], "enumerate")) {
        char endpoints[IT_MAX_CANDIDATES][IT_ENDPOINT_BYTES];
        size_t count = it_enumerate(endpoints, IT_MAX_CANDIDATES);
        printf("CANDIDATES=%lu\n", (unsigned long)count);
#ifndef _WIN32
        curl_global_cleanup();
#endif
        return 0;
    }
    if (argc != 3) return 2;
    kind = !strcmp(argv[1], "serial") ? IT_SERIAL : IT_WEBSOCKET;
    loops = !strcmp(argv[1], "ws-idle") ? 10 : 100;
    if (!it_open(&transport, kind, argv[2], 115200, 1500, 20)) {
        printf("OPENFAIL %s\n", it_error(&transport));
#ifndef _WIN32
        curl_global_cleanup();
#endif
        return 3;
    }
    for (i = 0; i < loops; ++i) {
        n = it_read(&transport, data, sizeof data);
        if (n < 0) { printf("CLOSED %s\n", it_error(&transport)); break; }
        if (n) {
            int j;
            for (j = 0; j < n; ++j) printf("%02x", data[j]);
            total += n;
        }
    }
    it_close(&transport);
    printf(" TOTAL=%d\n", total);
    result = total ? 0 : 4;
#ifndef _WIN32
    curl_global_cleanup();
#endif
    return result;
}