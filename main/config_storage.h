#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#define CFG_SSID_LEN 33
#define CFG_PASS_LEN 65
#define CFG_USER_LEN 33
#define CFG_EAP_ID_LEN 65
#define CFG_PORT_FWD_MAX 5
#define CFG_TS_AUTH_KEY_LEN 128
#define CFG_TS_NAME_LEN 32
#define CFG_TS_CTRL_HOST_LEN 96
#define CFG_TS_MAX_PEERS_MIN 1
#define CFG_TS_MAX_PEERS_MAX 64
#define CFG_TS_MAX_PEERS_DEFAULT 8
#define CFG_HOSTNAME_LEN 32
#define CFG_STA_RETRY_MIN 1
#define CFG_STA_RETRY_MAX_LIMIT 20
#define CFG_STA_RETRY_DEFAULT 5
#define CFG_STA_BACKOFF_MIN_S 1
#define CFG_STA_BACKOFF_MAX_S 300
#define CFG_STA_BACKOFF_DEFAULT_S 30
#define CFG_SCHED_TZ_LEN 64
#define CFG_SCHED_RULES_MAX 8
#define CFG_SCHED_NOTE_LEN 32
#define CFG_SCHED_TZ_DEFAULT "WET0WEST,M3.5.0/1,M10.5.0"

typedef enum {
    NET_MODE_REPEATER = 0,
    NET_MODE_TS_GATEWAY = 1,
} network_mode_t;

typedef enum {
    SCHED_MODE_ALWAYS_ON = 0,
    SCHED_MODE_SCHEDULED = 1,
    SCHED_MODE_MANUAL_OFF = 2,
} scheduler_mode_t;

typedef struct {
    bool enabled;
    uint8_t days;       /* bit0=Mon ... bit6=Sun */
    uint16_t start_min; /* minutes since midnight */
    uint16_t end_min;   /* minutes since midnight, same-day only */
    bool ap_enabled;
    bool sta_enabled;
    bool tailscale_enabled;
    char note[CFG_SCHED_NOTE_LEN];
} scheduler_rule_t;

typedef struct {
    bool     enabled;
    uint8_t  proto;       // 0=TCP, 1=UDP
    uint16_t ext_port;
    uint32_t int_ip;      // destination IP (network byte order)
    uint16_t int_port;
} port_fwd_rule_t;

typedef struct {
    bool enabled;
    char auth_key[CFG_TS_AUTH_KEY_LEN];
    char device_name[CFG_TS_NAME_LEN];
    char control_host[CFG_TS_CTRL_HOST_LEN];
    bool enable_derp;
    bool enable_disco;
    bool enable_stun;
    uint8_t max_peers;
    bool expose_web_ui;
    bool expose_lan;
    network_mode_t net_mode;
    char advertise_cidr[CFG_TS_CTRL_HOST_LEN]; /* e.g. "192.168.24.0/24" */
} tailscale_config_t;

/* Log levels (esp_log_level_t mirror, kept independent of header dependency). */
#define CFG_LOG_LEVEL_NONE    0
#define CFG_LOG_LEVEL_ERROR   1
#define CFG_LOG_LEVEL_WARN    2
#define CFG_LOG_LEVEL_INFO    3
#define CFG_LOG_LEVEL_DEBUG   4
#define CFG_LOG_LEVEL_VERBOSE 5

typedef struct {
    char sta_ssid[CFG_SSID_LEN];
    char sta_pass[CFG_PASS_LEN];
    char ap_ssid[CFG_SSID_LEN];
    char ap_pass[CFG_PASS_LEN];
    uint8_t ap_channel;
    uint8_t ap_max_conn;
    bool ap_hide_ssid;
    uint8_t sta_retry_max;
    uint16_t sta_backoff_s;
    char hostname[CFG_HOSTNAME_LEN];
    uint8_t sta_mac[6];
    bool sta_mac_custom;
    uint8_t ap_mac[6];
    bool ap_mac_custom;
    char web_user[CFG_USER_LEN];
    char web_pass[CFG_PASS_LEN];
    // WPA2-Enterprise (EAP-PEAP/TTLS)
    bool sta_eap_enabled;
    char sta_eap_identity[CFG_EAP_ID_LEN];
    char sta_eap_username[CFG_USER_LEN];
    char sta_eap_password[CFG_PASS_LEN];
    // Port forwarding
    port_fwd_rule_t port_fwd[CFG_PORT_FWD_MAX];
    // Tailscale/MicroLink
    tailscale_config_t tailscale;
    // Runtime log levels
    uint8_t log_level_global;     // CFG_LOG_LEVEL_* — default INFO
    uint8_t log_level_microlink;  // CFG_LOG_LEVEL_* — default WARN
    // Scheduler / automation
    char sched_tz[CFG_SCHED_TZ_LEN];
    scheduler_mode_t sched_mode;
    scheduler_rule_t sched_rules[CFG_SCHED_RULES_MAX];
} repeater_config_t;

esp_err_t config_storage_init(void);
esp_err_t config_storage_load(repeater_config_t *config);
esp_err_t config_storage_save(const repeater_config_t *config);
esp_err_t config_storage_erase(void);
void config_storage_set_defaults(repeater_config_t *config);

/* Apply runtime log levels from config to the ESP-IDF log subsystem. */
void config_storage_apply_log_levels(const repeater_config_t *config);
