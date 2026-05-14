#pragma once

#include "esp_err.h"
#include "esp_wifi_types.h"
#include "esp_netif.h"
#include "config_storage.h"
#include "dns_server.h"

#define WIFI_SCAN_MAX_AP 20

typedef struct {
    bool sta_connected;
    int8_t sta_rssi;
    uint32_t sta_ip;
    uint8_t ap_client_count;
    char sta_ssid[CFG_SSID_LEN];
    uint8_t sta_mac[6];
    uint8_t ap_mac[6];
    uint8_t sta_retry_count;
    bool sta_recovery;
    bool sta_paused;
    bool sta_scheduler_enabled;
    bool ap_enabled;
    uint16_t sta_next_retry_s;
} wifi_status_t;

esp_err_t wifi_manager_init(repeater_config_t *config);
esp_err_t wifi_manager_start(void);
esp_err_t wifi_manager_reconfigure(repeater_config_t *config);
esp_err_t wifi_manager_scan(wifi_ap_record_t *ap_records, uint16_t *ap_count);
void wifi_manager_get_status(wifi_status_t *status);
esp_err_t wifi_manager_pause_sta(void);
esp_err_t wifi_manager_resume_sta(void);
esp_err_t wifi_manager_set_sta_scheduler_enabled(bool enabled);
esp_err_t wifi_manager_set_ap_enabled(bool enabled);
void wifi_manager_set_dns_handle(dns_server_handle_t handle);
void wifi_manager_apply_port_forwarding(void);
esp_err_t wifi_manager_set_subnet_routing(bool enable);
bool wifi_manager_is_subnet_routing(void);

typedef struct {
    bool success;
    uint32_t elapsed_ms;
    uint32_t addr;
} ping_result_t;

esp_err_t wifi_manager_ping(const char *target, ping_result_t *result);
