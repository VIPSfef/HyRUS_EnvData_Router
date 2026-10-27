#ifndef TB_CONFIG_H
#define TB_CONFIG_H

#include <stddef.h>

enum { CFG_AUTH_TOKEN_MAX = 1024, CFG_AUTH_TOKEN_BYTES = CFG_AUTH_TOKEN_MAX + 1 };

typedef struct tb_config {
    char url[2048], stream[65], session[65], token_env[65];
    double origin[3];
    unsigned send_interval_ms, connect_timeout_ms, io_timeout_ms, max_windows;
    unsigned refresh_ms, idle_warn_ms, ack_warn_ms;
    int summary;
    char input_mode[16], serial_port[256], websocket_url[2048], unit_map[1024];
    unsigned serial_baud, auto_scan, probe_ms, link_timeout_ms, reconnect_ms;
    unsigned target_system, target_component, system_time_uncert_us, duration_ms;
    unsigned origin_auto;
    double accel_bound_mps2;
    char udp_bind[64];
    unsigned udp_port, udp_enabled;
    double spatial_step_m;
    unsigned spatial_gap_ms;
    int spatial_enabled, spatial_adaptive;
    /* MAVLink forwarding destination, independent of the UDP input listener. */
    unsigned qgc_enabled;
    char qgc_host[64];
    unsigned qgc_port;
    /* Independent status reports; zero preserves legacy measurement-only HTTP. */
    unsigned status_interval_ms;
    /* Opt-in serial message interval requests for prioritized telemetry. */
    unsigned mavlink_rates_enabled;
    /* Latest-state UDP backup: auto host follows HTTP; zero port is HTTP+1. */
    unsigned udp_backup_enabled;
    char udp_backup_host[256];
    unsigned udp_backup_port;
    /* Retained HTTP/TCP measurement queue budget, in MiB; never disk-backed. */
    unsigned tcp_buffer_max_mb;
    /* Direct credential, preferred over token_env. Never render this value. */
    char token[CFG_AUTH_TOKEN_BYTES];
    /* Shared completion-to-next-request gap for every HTTP POST. */
    unsigned http_min_interval_ms;
} tb_config;

enum {
    CFG_MIB_BYTES = 1024 * 1024,
    CFG_TCP_BUFFER_DEFAULT_MB = 1024,
    CFG_TCP_BUFFER_MAX_MB = 65536,
    CFG_HTTP_MIN_INTERVAL_MS = 500
};

/* Numeric parsing/formatting requires the C numeric locale. Errors never echo
 * supplied values. cfg_set and cfg_load leave the configuration unchanged on
 * failure; save validates first and atomically replaces only a complete file.
 * Paths use UTF-8 on Windows. Direct tokens are saved with current-user DPAPI
 * protection on Windows and owner-only file permissions on POSIX. */
void cfg_defaults(tb_config *config);
int cfg_set(tb_config *config, const char *key, const char *value,
            char *err, size_t err_cap);
int cfg_validate(const tb_config *config, unsigned max_windows,
                 char *err, size_t err_cap);
int cfg_load(const char *path, tb_config *config, unsigned max_windows,
             char *err, size_t err_cap);
int cfg_save(const char *path, const tb_config *config, unsigned max_windows,
             char *err, size_t err_cap);
/* Stable zero-based registry order is shared by saved files and the editor.
 * Invalid indices have no name or URL type. Value formatting returns zero for
 * invalid arguments or truncation and always terminates a nonempty buffer.
 * URL credentials/queries and direct tokens are hidden unless reveal_url is
 * explicitly true for a private editor buffer. */
unsigned cfg_field_count(void);
const char *cfg_field_name(unsigned index);
int cfg_field_is_url(unsigned index);
int cfg_field_is_secret(unsigned index);
/* Returns the direct token when set, otherwise the named environment value.
 * The result is borrowed and must never be displayed or logged. */
const char *cfg_auth_token(const tb_config *config);
int cfg_field_value(const tb_config *config, unsigned index, char *out,
                    size_t cap, int reveal_url);
/* Displays only scheme/host/path. All query/fragment content is hidden. */
void cfg_safe_url(const char *url, char *out, size_t cap);
/* Editor drafts may have incomplete schemes/hosts. Show the draft without
 * validation while applying the same credential/query/fragment redaction. */
void cfg_edit_url(const char *url, char *out, size_t cap);
/* Resolve configuration only, without DNS/network I/O. IPv6 output is
 * unbracketed for getaddrinfo. Failure clears outputs and never echoes URLs. */
int cfg_udp_backup_destination(const tb_config *config,char *host,size_t host_cap,
                               unsigned *port,char *err,size_t err_cap);

/* Lookup an explicitly configured key unit; leaves out unchanged when absent. */
int cfg_unit_for_key(const tb_config *config, const char *key,
                     char *out, size_t cap);

#endif
