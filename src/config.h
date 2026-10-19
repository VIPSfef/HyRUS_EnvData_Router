#ifndef TB_CONFIG_H
#define TB_CONFIG_H

#include <stddef.h>

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
} tb_config;

/* Numeric parsing/formatting requires the C numeric locale. Errors never echo
 * supplied values. cfg_set and cfg_load leave the configuration unchanged on
 * failure; save validates first and atomically replaces only a complete file.
 * Paths use UTF-8 on Windows. Authorization is an environment VARIABLE NAME,
 * never a bearer token value. Loaded/saved settings apply on process restart. */
void cfg_defaults(tb_config *config);
int cfg_set(tb_config *config, const char *key, const char *value,
            char *err, size_t err_cap);
int cfg_validate(const tb_config *config, unsigned max_windows,
                 char *err, size_t err_cap);
int cfg_load(const char *path, tb_config *config, unsigned max_windows,
             char *err, size_t err_cap);
int cfg_save(const char *path, const tb_config *config, unsigned max_windows,
             char *err, size_t err_cap);
/* Displays only scheme/host/path. All query/fragment content is hidden. */
void cfg_safe_url(const char *url, char *out, size_t cap);

/* Lookup an explicitly configured key unit; leaves out unchanged when absent. */
int cfg_unit_for_key(const tb_config *config, const char *key,
                     char *out, size_t cap);

#endif
