#include <string.h>
#include "config_storage.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "config_storage";
#define NVS_NAMESPACE "repeater_cfg"

static bool hostname_valid(const char *value)
{
    size_t len = strlen(value);
    if (len == 0 || len >= CFG_HOSTNAME_LEN) return false;
    if (value[0] == '-' || value[len - 1] == '-') return false;

    for (size_t i = 0; i < len; i++) {
        char c = value[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-') {
            continue;
        }
        return false;
    }
    return true;
}

static bool scheduler_tz_valid(const char *value)
{
    size_t len = strlen(value);
    if (len == 0 || len >= CFG_SCHED_TZ_LEN) return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)value[i];
        if (c < 0x21 || c > 0x7e) return false;
    }
    return true;
}

static bool scheduler_rule_valid(const scheduler_rule_t *rule)
{
    if (!rule->enabled) return true;
    if ((rule->days & 0x7f) == 0) return false;
    if (rule->start_min >= 1440 || rule->end_min > 1440) return false;
    if (rule->start_min >= rule->end_min) return false;
    return true;
}

static bool custom_mac_valid(const uint8_t mac[6])
{
    bool all_zero = true;
    bool all_ff = true;
    for (int i = 0; i < 6; i++) {
        if (mac[i] != 0x00) all_zero = false;
        if (mac[i] != 0xff) all_ff = false;
    }
    if (all_zero || all_ff) return false;
    if (mac[0] & 0x01) return false; /* multicast */
    return true;
}

esp_err_t config_storage_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

void config_storage_set_defaults(repeater_config_t *config)
{
    memset(config, 0, sizeof(repeater_config_t));
    strlcpy(config->ap_ssid, "ESP32-Repeater", sizeof(config->ap_ssid));
    strlcpy(config->ap_pass, "12345678", sizeof(config->ap_pass));
    config->ap_channel = 1;
    config->ap_max_conn = 4;
    config->ap_hide_ssid = false;
    config->sta_retry_max = CFG_STA_RETRY_DEFAULT;
    config->sta_backoff_s = CFG_STA_BACKOFF_DEFAULT_S;
    strlcpy(config->hostname, "esp32-repeater", sizeof(config->hostname));
    strlcpy(config->web_user, "admin", sizeof(config->web_user));
    strlcpy(config->web_pass, "admin", sizeof(config->web_pass));
    config->tailscale.enabled = false;
    config->tailscale.auth_key[0] = '\0';
    strlcpy(config->tailscale.device_name, "esp32-repeater", sizeof(config->tailscale.device_name));
    config->tailscale.control_host[0] = '\0';
    config->tailscale.enable_derp = true;
    config->tailscale.enable_disco = true;
    config->tailscale.enable_stun = true;
    config->tailscale.max_peers = CFG_TS_MAX_PEERS_DEFAULT;
    config->tailscale.expose_web_ui = false;
    config->tailscale.expose_lan = false;
    config->tailscale.net_mode = NET_MODE_REPEATER;
    strlcpy(config->tailscale.advertise_cidr, "192.168.24.0/24", sizeof(config->tailscale.advertise_cidr));
    /* Defaults runtime log levels: global=INFO, MicroLink=WARN to keep CPU
     * available for Tailscale handshake/transfer. */
    config->log_level_global = CFG_LOG_LEVEL_INFO;
    config->log_level_microlink = CFG_LOG_LEVEL_WARN;
    strlcpy(config->sched_tz, CFG_SCHED_TZ_DEFAULT, sizeof(config->sched_tz));
    config->sched_mode = SCHED_MODE_ALWAYS_ON;
    for (int i = 0; i < CFG_SCHED_RULES_MAX; i++) {
        config->sched_rules[i].ap_enabled = true;
        config->sched_rules[i].sta_enabled = true;
        config->sched_rules[i].tailscale_enabled = true;
    }
}

esp_err_t config_storage_load(repeater_config_t *config)
{
    config_storage_set_defaults(config);

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "NVS namespace not found, using defaults");
        return ESP_ERR_NOT_FOUND;
    }

    size_t len;

    len = sizeof(config->sta_ssid);
    if (nvs_get_str(handle, "sta_ssid", config->sta_ssid, &len) != ESP_OK) {
        config->sta_ssid[0] = '\0';
    }

    len = sizeof(config->sta_pass);
    if (nvs_get_str(handle, "sta_pass", config->sta_pass, &len) != ESP_OK) {
        config->sta_pass[0] = '\0';
    }

    len = sizeof(config->ap_ssid);
    nvs_get_str(handle, "ap_ssid", config->ap_ssid, &len);

    len = sizeof(config->ap_pass);
    nvs_get_str(handle, "ap_pass", config->ap_pass, &len);

    uint8_t val;
    if (nvs_get_u8(handle, "ap_channel", &val) == ESP_OK) config->ap_channel = val;
    if (nvs_get_u8(handle, "ap_max_conn", &val) == ESP_OK) config->ap_max_conn = val;
    if (nvs_get_u8(handle, "ap_hidden", &val) == ESP_OK) config->ap_hide_ssid = val != 0;
    if (nvs_get_u8(handle, "sta_retry", &val) == ESP_OK) config->sta_retry_max = val;
    uint16_t val16;
    if (nvs_get_u16(handle, "sta_bkoff", &val16) == ESP_OK) config->sta_backoff_s = val16;

    len = sizeof(config->hostname);
    nvs_get_str(handle, "hostname", config->hostname, &len);

    len = sizeof(config->sta_mac);
    nvs_get_blob(handle, "sta_mac", config->sta_mac, &len);
    if (nvs_get_u8(handle, "sta_mac_en", &val) == ESP_OK) config->sta_mac_custom = val != 0;

    len = sizeof(config->ap_mac);
    nvs_get_blob(handle, "ap_mac", config->ap_mac, &len);
    if (nvs_get_u8(handle, "ap_mac_en", &val) == ESP_OK) config->ap_mac_custom = val != 0;

    len = sizeof(config->web_user);
    nvs_get_str(handle, "web_user", config->web_user, &len);

    len = sizeof(config->web_pass);
    nvs_get_str(handle, "web_pass", config->web_pass, &len);

    // EAP fields
    uint8_t eap_en = 0;
    if (nvs_get_u8(handle, "eap_enabled", &eap_en) == ESP_OK) config->sta_eap_enabled = eap_en;

    len = sizeof(config->sta_eap_identity);
    nvs_get_str(handle, "eap_identity", config->sta_eap_identity, &len);

    len = sizeof(config->sta_eap_username);
    nvs_get_str(handle, "eap_user", config->sta_eap_username, &len);

    len = sizeof(config->sta_eap_password);
    nvs_get_str(handle, "eap_pass", config->sta_eap_password, &len);

    // Port forwarding (stored as blob)
    len = sizeof(config->port_fwd);
    nvs_get_blob(handle, "port_fwd", config->port_fwd, &len);

    // Tailscale fields. Keep key names short for NVS compatibility.
    if (nvs_get_u8(handle, "ts_enabled", &val) == ESP_OK) config->tailscale.enabled = val != 0;

    len = sizeof(config->tailscale.auth_key);
    if (nvs_get_str(handle, "ts_auth", config->tailscale.auth_key, &len) != ESP_OK) {
        config->tailscale.auth_key[0] = '\0';
    }

    len = sizeof(config->tailscale.device_name);
    nvs_get_str(handle, "ts_name", config->tailscale.device_name, &len);

    len = sizeof(config->tailscale.control_host);
    nvs_get_str(handle, "ts_ctrl", config->tailscale.control_host, &len);

    if (nvs_get_u8(handle, "ts_derp", &val) == ESP_OK) config->tailscale.enable_derp = val != 0;
    if (nvs_get_u8(handle, "ts_disco", &val) == ESP_OK) config->tailscale.enable_disco = val != 0;
    if (nvs_get_u8(handle, "ts_stun", &val) == ESP_OK) config->tailscale.enable_stun = val != 0;
    if (nvs_get_u8(handle, "ts_peers", &val) == ESP_OK) config->tailscale.max_peers = val;
    if (nvs_get_u8(handle, "ts_web", &val) == ESP_OK) config->tailscale.expose_web_ui = val != 0;
    if (nvs_get_u8(handle, "ts_lan", &val) == ESP_OK) config->tailscale.expose_lan = val != 0;
    if (nvs_get_u8(handle, "ts_mode", &val) == ESP_OK) config->tailscale.net_mode = (network_mode_t)val;
    len = sizeof(config->tailscale.advertise_cidr);
    nvs_get_str(handle, "ts_cidr", config->tailscale.advertise_cidr, &len);

    if (config->tailscale.max_peers < CFG_TS_MAX_PEERS_MIN ||
        config->tailscale.max_peers > CFG_TS_MAX_PEERS_MAX) {
        ESP_LOGW(TAG, "Invalid Tailscale max_peers=%u, using default",
                 config->tailscale.max_peers);
        config->tailscale.max_peers = CFG_TS_MAX_PEERS_DEFAULT;
    }
    if (config->tailscale.net_mode != NET_MODE_REPEATER &&
        config->tailscale.net_mode != NET_MODE_TS_GATEWAY) {
        ESP_LOGW(TAG, "Invalid Tailscale net_mode=%u, using repeater mode",
                 (unsigned)config->tailscale.net_mode);
        config->tailscale.net_mode = NET_MODE_REPEATER;
    }
    if (config->tailscale.expose_lan) {
        if (config->tailscale.net_mode != NET_MODE_TS_GATEWAY) {
            ESP_LOGW(TAG, "LAN route advertising forces Tailscale Gateway/SNAT mode");
        }
        config->tailscale.net_mode = NET_MODE_TS_GATEWAY;
    } else {
        config->tailscale.net_mode = NET_MODE_REPEATER;
    }

    if (nvs_get_u8(handle, "log_global", &val) == ESP_OK) config->log_level_global = val;
    if (nvs_get_u8(handle, "log_ml", &val) == ESP_OK) config->log_level_microlink = val;
    len = sizeof(config->sched_tz);
    nvs_get_str(handle, "sched_tz", config->sched_tz, &len);
    if (nvs_get_u8(handle, "sched_mode", &val) == ESP_OK) {
        config->sched_mode = (scheduler_mode_t)val;
    }
    for (int i = 0; i < CFG_SCHED_RULES_MAX; i++) {
        char key[16];
        scheduler_rule_t *rule = &config->sched_rules[i];

        snprintf(key, sizeof(key), "sr%d_en", i);
        if (nvs_get_u8(handle, key, &val) == ESP_OK) rule->enabled = val != 0;
        snprintf(key, sizeof(key), "sr%d_days", i);
        if (nvs_get_u8(handle, key, &val) == ESP_OK) rule->days = val & 0x7f;
        snprintf(key, sizeof(key), "sr%d_start", i);
        if (nvs_get_u16(handle, key, &val16) == ESP_OK) rule->start_min = val16;
        snprintf(key, sizeof(key), "sr%d_end", i);
        if (nvs_get_u16(handle, key, &val16) == ESP_OK) rule->end_min = val16;
        snprintf(key, sizeof(key), "sr%d_ap", i);
        if (nvs_get_u8(handle, key, &val) == ESP_OK) rule->ap_enabled = val != 0;
        snprintf(key, sizeof(key), "sr%d_sta", i);
        if (nvs_get_u8(handle, key, &val) == ESP_OK) rule->sta_enabled = val != 0;
        snprintf(key, sizeof(key), "sr%d_ts", i);
        if (nvs_get_u8(handle, key, &val) == ESP_OK) rule->tailscale_enabled = val != 0;
        snprintf(key, sizeof(key), "sr%d_note", i);
        len = sizeof(rule->note);
        nvs_get_str(handle, key, rule->note, &len);
    }
    if (config->log_level_global > CFG_LOG_LEVEL_VERBOSE) {
        config->log_level_global = CFG_LOG_LEVEL_INFO;
    }
    if (config->log_level_microlink > CFG_LOG_LEVEL_VERBOSE) {
        config->log_level_microlink = CFG_LOG_LEVEL_WARN;
    }
    if (config->sta_retry_max < CFG_STA_RETRY_MIN ||
        config->sta_retry_max > CFG_STA_RETRY_MAX_LIMIT) {
        config->sta_retry_max = CFG_STA_RETRY_DEFAULT;
    }
    if (config->sta_backoff_s < CFG_STA_BACKOFF_MIN_S ||
        config->sta_backoff_s > CFG_STA_BACKOFF_MAX_S) {
        config->sta_backoff_s = CFG_STA_BACKOFF_DEFAULT_S;
    }
    if (!hostname_valid(config->hostname)) {
        ESP_LOGW(TAG, "Invalid hostname in NVS, using default");
        strlcpy(config->hostname, "esp32-repeater", sizeof(config->hostname));
    }
    if (config->sta_mac_custom && !custom_mac_valid(config->sta_mac)) {
        ESP_LOGW(TAG, "Invalid custom STA MAC in NVS, disabling");
        config->sta_mac_custom = false;
    }
    if (config->ap_mac_custom && !custom_mac_valid(config->ap_mac)) {
        ESP_LOGW(TAG, "Invalid custom AP MAC in NVS, disabling");
        config->ap_mac_custom = false;
    }
    if (!scheduler_tz_valid(config->sched_tz)) {
        ESP_LOGW(TAG, "Invalid scheduler timezone in NVS, using default");
        strlcpy(config->sched_tz, CFG_SCHED_TZ_DEFAULT, sizeof(config->sched_tz));
    }
    if (config->sched_mode != SCHED_MODE_ALWAYS_ON &&
        config->sched_mode != SCHED_MODE_SCHEDULED &&
        config->sched_mode != SCHED_MODE_MANUAL_OFF) {
        ESP_LOGW(TAG, "Invalid scheduler mode in NVS, using always-on");
        config->sched_mode = SCHED_MODE_ALWAYS_ON;
    }
    for (int i = 0; i < CFG_SCHED_RULES_MAX; i++) {
        if (!scheduler_rule_valid(&config->sched_rules[i])) {
            ESP_LOGW(TAG, "Invalid scheduler rule %d in NVS, disabling", i);
            memset(&config->sched_rules[i], 0, sizeof(config->sched_rules[i]));
        }
    }

    nvs_close(handle);
    ESP_LOGI(TAG, "Config loaded: STA='%s' AP='%s' CH=%d hostname='%s'",
             config->sta_ssid, config->ap_ssid, config->ap_channel,
             config->hostname);
    return ESP_OK;
}

esp_err_t config_storage_save(const repeater_config_t *config)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return ret;
    }

    nvs_set_str(handle, "sta_ssid", config->sta_ssid);
    nvs_set_str(handle, "sta_pass", config->sta_pass);
    nvs_set_str(handle, "ap_ssid", config->ap_ssid);
    nvs_set_str(handle, "ap_pass", config->ap_pass);
    nvs_set_u8(handle, "ap_channel", config->ap_channel);
    nvs_set_u8(handle, "ap_max_conn", config->ap_max_conn);
    nvs_set_u8(handle, "ap_hidden", config->ap_hide_ssid ? 1 : 0);
    nvs_set_u8(handle, "sta_retry", config->sta_retry_max);
    nvs_set_u16(handle, "sta_bkoff", config->sta_backoff_s);
    nvs_set_str(handle, "hostname", config->hostname);
    nvs_set_blob(handle, "sta_mac", config->sta_mac, sizeof(config->sta_mac));
    nvs_set_u8(handle, "sta_mac_en", config->sta_mac_custom ? 1 : 0);
    nvs_set_blob(handle, "ap_mac", config->ap_mac, sizeof(config->ap_mac));
    nvs_set_u8(handle, "ap_mac_en", config->ap_mac_custom ? 1 : 0);
    nvs_set_str(handle, "web_user", config->web_user);
    nvs_set_str(handle, "web_pass", config->web_pass);

    // EAP fields
    nvs_set_u8(handle, "eap_enabled", config->sta_eap_enabled ? 1 : 0);
    nvs_set_str(handle, "eap_identity", config->sta_eap_identity);
    nvs_set_str(handle, "eap_user", config->sta_eap_username);
    nvs_set_str(handle, "eap_pass", config->sta_eap_password);

    // Port forwarding (as blob)
    nvs_set_blob(handle, "port_fwd", config->port_fwd, sizeof(config->port_fwd));

    // Tailscale fields. Do not log auth_key or other secrets.
    nvs_set_u8(handle, "ts_enabled", config->tailscale.enabled ? 1 : 0);
    nvs_set_str(handle, "ts_auth", config->tailscale.auth_key);
    nvs_set_str(handle, "ts_name", config->tailscale.device_name);
    nvs_set_str(handle, "ts_ctrl", config->tailscale.control_host);
    nvs_set_u8(handle, "ts_derp", config->tailscale.enable_derp ? 1 : 0);
    nvs_set_u8(handle, "ts_disco", config->tailscale.enable_disco ? 1 : 0);
    nvs_set_u8(handle, "ts_stun", config->tailscale.enable_stun ? 1 : 0);
    nvs_set_u8(handle, "ts_peers", config->tailscale.max_peers);
    nvs_set_u8(handle, "ts_web", config->tailscale.expose_web_ui ? 1 : 0);
    nvs_set_u8(handle, "ts_lan", config->tailscale.expose_lan ? 1 : 0);
    nvs_set_u8(handle, "ts_mode", (uint8_t)config->tailscale.net_mode);
    nvs_set_str(handle, "ts_cidr", config->tailscale.advertise_cidr);

    nvs_set_u8(handle, "log_global", config->log_level_global);
    nvs_set_u8(handle, "log_ml", config->log_level_microlink);
    nvs_set_str(handle, "sched_tz", config->sched_tz);
    nvs_set_u8(handle, "sched_mode", (uint8_t)config->sched_mode);
    for (int i = 0; i < CFG_SCHED_RULES_MAX; i++) {
        char key[16];
        const scheduler_rule_t *rule = &config->sched_rules[i];

        snprintf(key, sizeof(key), "sr%d_en", i);
        nvs_set_u8(handle, key, rule->enabled ? 1 : 0);
        snprintf(key, sizeof(key), "sr%d_days", i);
        nvs_set_u8(handle, key, rule->days & 0x7f);
        snprintf(key, sizeof(key), "sr%d_start", i);
        nvs_set_u16(handle, key, rule->start_min);
        snprintf(key, sizeof(key), "sr%d_end", i);
        nvs_set_u16(handle, key, rule->end_min);
        snprintf(key, sizeof(key), "sr%d_ap", i);
        nvs_set_u8(handle, key, rule->ap_enabled ? 1 : 0);
        snprintf(key, sizeof(key), "sr%d_sta", i);
        nvs_set_u8(handle, key, rule->sta_enabled ? 1 : 0);
        snprintf(key, sizeof(key), "sr%d_ts", i);
        nvs_set_u8(handle, key, rule->tailscale_enabled ? 1 : 0);
        snprintf(key, sizeof(key), "sr%d_note", i);
        nvs_set_str(handle, key, rule->note);
    }

    ret = nvs_commit(handle);
    nvs_close(handle);

    ESP_LOGI(TAG, "Config saved: STA='%s' AP='%s' CH=%d hostname='%s'",
             config->sta_ssid, config->ap_ssid, config->ap_channel,
             config->hostname);
    return ret;
}

esp_err_t config_storage_erase(void)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) return ret;

    ret = nvs_erase_all(handle);
    nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "Config erased");
    return ret;
}

void config_storage_apply_log_levels(const repeater_config_t *config)
{
    if (!config) return;

    esp_log_level_t global = (esp_log_level_t)config->log_level_global;
    esp_log_level_t ml     = (esp_log_level_t)config->log_level_microlink;
    if (global > ESP_LOG_VERBOSE) global = ESP_LOG_INFO;
    if (ml > ESP_LOG_VERBOSE) ml = ESP_LOG_WARN;

    /* Apply global first; then override the heavy MicroLink/WireGuard tags. */
    esp_log_level_set("*", global);

    static const char *ml_tags[] = {
        "ml_coord", "ml_h2", "ml_noise", "ml_wg_mgr", "ml_derp",
        "ml_net_io", "ml_disco", "ml_stun", "microlink",
        "wireguardif", "wireguard",
    };
    for (size_t i = 0; i < sizeof(ml_tags) / sizeof(ml_tags[0]); i++) {
        esp_log_level_set(ml_tags[i], ml);
    }

    ESP_LOGI(TAG, "Log levels applied: global=%d microlink=%d",
             (int)global, (int)ml);
}
