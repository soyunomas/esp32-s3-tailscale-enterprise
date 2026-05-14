#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "web_server.h"
#include "wifi_manager.h"
#include "config_storage.h"
#include "tailscale_manager.h"
#include "scheduler_manager.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi_ap_get_sta_list.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_heap_caps.h"
#include "lwip/inet.h"
#include "mbedtls/base64.h"
#include "log_buffer.h"

static const char *TAG = "web_server";
static httpd_handle_t s_server = NULL;
static repeater_config_t *s_config = NULL;

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");
extern const uint8_t styles_css_start[] asm("_binary_styles_css_start");
extern const uint8_t styles_css_end[]   asm("_binary_styles_css_end");
extern const uint8_t app_js_start[]     asm("_binary_app_js_start");
extern const uint8_t app_js_end[]       asm("_binary_app_js_end");

// --- Utility: escape a string for JSON ---
static int json_escape(char *dst, size_t dst_size, const char *src)
{
    size_t j = 0;
    for (size_t i = 0; src[i] && j < dst_size - 1; i++) {
        if (src[i] == '"' || src[i] == '\\') {
            if (j + 2 >= dst_size) break;
            dst[j++] = '\\';
        }
        dst[j++] = src[i];
    }
    dst[j] = '\0';
    return (int)j;
}

// --- Basic Auth helper ---
static bool require_auth(httpd_req_t *req)
{
    char auth_hdr[256];
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_hdr, sizeof(auth_hdr)) != ESP_OK) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP32 Repeater\"");
        httpd_resp_sendstr(req, "{\"error\":\"Authentication required\"}");
        return false;
    }

    if (strncmp(auth_hdr, "Basic ", 6) != 0) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_sendstr(req, "{\"error\":\"Invalid auth method\"}");
        return false;
    }

    unsigned char decoded[128];
    size_t decoded_len = 0;
    if (mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decoded_len,
                               (const unsigned char *)(auth_hdr + 6),
                               strlen(auth_hdr + 6)) != 0) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_sendstr(req, "{\"error\":\"Invalid credentials\"}");
        return false;
    }
    decoded[decoded_len] = '\0';

    char *colon = strchr((char *)decoded, ':');
    if (!colon) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_sendstr(req, "{\"error\":\"Invalid credentials\"}");
        return false;
    }
    *colon = '\0';
    const char *user = (const char *)decoded;
    const char *pass = colon + 1;

    if (strcmp(user, s_config->web_user) != 0 || strcmp(pass, s_config->web_pass) != 0) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP32 Repeater\"");
        httpd_resp_sendstr(req, "{\"error\":\"Invalid credentials\"}");
        return false;
    }

    return true;
}

// --- Static file handlers ---

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)index_html_start,
                    index_html_end - index_html_start);
    return ESP_OK;
}

static esp_err_t css_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/css");
    httpd_resp_send(req, (const char *)styles_css_start,
                    styles_css_end - styles_css_start);
    return ESP_OK;
}

static esp_err_t js_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_send(req, (const char *)app_js_start,
                    app_js_end - app_js_start);
    return ESP_OK;
}

// --- API handlers ---

static esp_err_t api_status_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    wifi_status_t status;
    wifi_manager_get_status(&status);

    char ip_str[16];
    esp_ip4_addr_t ip = { .addr = status.sta_ip };
    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip));

    esp_netif_ip_info_t ap_ip_info;
    esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"), &ap_ip_info);
    char ap_ip_str[16];
    snprintf(ap_ip_str, sizeof(ap_ip_str), IPSTR, IP2STR(&ap_ip_info.ip));

    uint32_t free_heap = esp_get_free_heap_size();
    uint32_t uptime_sec = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS / 1000);

    char esc_ssid[68];
    json_escape(esc_ssid, sizeof(esc_ssid), status.sta_ssid);

    char sta_mac[18];
    char ap_mac[18];
    snprintf(sta_mac, sizeof(sta_mac), MACSTR, MAC2STR(status.sta_mac));
    snprintf(ap_mac, sizeof(ap_mac), MACSTR, MAC2STR(status.ap_mac));

    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"sta_connected\":%s,\"sta_ssid\":\"%s\",\"sta_rssi\":%d,"
        "\"sta_ip\":\"%s\",\"ap_clients\":%d,\"ap_ip\":\"%s\","
        "\"sta_mac\":\"%s\",\"ap_mac\":\"%s\","
        "\"sta_retry_count\":%u,\"sta_recovery\":%s,\"sta_paused\":%s,"
        "\"sta_scheduler_enabled\":%s,\"ap_enabled\":%s,"
        "\"sta_next_retry_s\":%u,\"free_heap\":%lu,\"uptime\":%lu}",
        status.sta_connected ? "true" : "false",
        esc_ssid, status.sta_rssi, ip_str,
        status.ap_client_count, ap_ip_str,
        sta_mac, ap_mac,
        status.sta_retry_count,
        status.sta_recovery ? "true" : "false",
        status.sta_paused ? "true" : "false",
        status.sta_scheduler_enabled ? "true" : "false",
        status.ap_enabled ? "true" : "false",
        status.sta_next_retry_s,
        (unsigned long)free_heap, (unsigned long)uptime_sec);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t api_wifi_state_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;

    wifi_status_t status;
    wifi_manager_get_status(&status);

    char ip_str[16];
    esp_ip4_addr_t ip = { .addr = status.sta_ip };
    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip));

    char esc_ssid[68];
    json_escape(esc_ssid, sizeof(esc_ssid), status.sta_ssid);

    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"connected\":%s,\"ssid\":\"%s\",\"ip\":\"%s\","
        "\"retry_count\":%u,\"recovery\":%s,\"paused\":%s,"
        "\"next_retry_s\":%u}",
        status.sta_connected ? "true" : "false",
        esc_ssid,
        ip_str,
        status.sta_retry_count,
        status.sta_recovery ? "true" : "false",
        status.sta_paused ? "true" : "false",
        status.sta_next_retry_s);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t api_wifi_pause_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"STA paused\"}");

    /* Pausing STA can drop this HTTP connection when the UI is accessed via
     * upstream IP. Let the response leave before disconnecting. */
    vTaskDelay(pdMS_TO_TICKS(500));
    wifi_manager_pause_sta();
    return ESP_OK;
}

static esp_err_t api_wifi_resume_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;

    esp_err_t ret = wifi_manager_resume_sta();
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "WiFi resume failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"STA resumed\"}");
    return ESP_OK;
}

static esp_err_t api_scan_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    wifi_ap_record_t ap_records[WIFI_SCAN_MAX_AP];
    uint16_t ap_count = 0;

    esp_err_t ret = wifi_manager_scan(ap_records, &ap_count);
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Scan failed");
        return ESP_FAIL;
    }

    // Build JSON array manually. Each entry ~100 bytes, max 20 entries
    char *buf = malloc(ap_count * 120 + 16);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    int pos = 0;
    pos += snprintf(buf + pos, 2, "[");
    for (int i = 0; i < ap_count; i++) {
        char esc_ssid[68];
        json_escape(esc_ssid, sizeof(esc_ssid), (const char *)ap_records[i].ssid);
        pos += snprintf(buf + pos, 120,
            "%s{\"ssid\":\"%s\",\"rssi\":%d,\"channel\":%d,\"auth\":%d}",
            i > 0 ? "," : "",
            esc_ssid, ap_records[i].rssi, ap_records[i].primary, ap_records[i].authmode);
    }
    pos += snprintf(buf + pos, 2, "]");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    free(buf);
    return ESP_OK;
}

static esp_err_t api_config_get_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    char esc_sta_ssid[68], esc_ap_ssid[68], esc_hostname[68];
    json_escape(esc_sta_ssid, sizeof(esc_sta_ssid), s_config->sta_ssid);
    json_escape(esc_ap_ssid, sizeof(esc_ap_ssid), s_config->ap_ssid);
    json_escape(esc_hostname, sizeof(esc_hostname), s_config->hostname);

    char esc_eap_id[130], esc_eap_user[68];
    json_escape(esc_eap_id, sizeof(esc_eap_id), s_config->sta_eap_identity);
    json_escape(esc_eap_user, sizeof(esc_eap_user), s_config->sta_eap_username);

    char sta_mac[18];
    char ap_mac[18];
    if (s_config->sta_mac_custom) {
        snprintf(sta_mac, sizeof(sta_mac), MACSTR, MAC2STR(s_config->sta_mac));
    } else {
        sta_mac[0] = '\0';
    }
    if (s_config->ap_mac_custom) {
        snprintf(ap_mac, sizeof(ap_mac), MACSTR, MAC2STR(s_config->ap_mac));
    } else {
        ap_mac[0] = '\0';
    }

    httpd_resp_set_type(req, "application/json");

    char buf[384];
    int len;

    len = snprintf(buf, sizeof(buf),
        "{\"sta_ssid\":\"%s\",\"sta_pass\":\"\","
        "\"ap_ssid\":\"%s\",\"ap_pass\":\"\","
        "\"ap_channel\":%d,\"ap_max_conn\":%d,\"ap_hide_ssid\":%s,",
        esc_sta_ssid, esc_ap_ssid,
        s_config->ap_channel, s_config->ap_max_conn,
        s_config->ap_hide_ssid ? "true" : "false");
    httpd_resp_send_chunk(req, buf, len);

    len = snprintf(buf, sizeof(buf),
        "\"sta_eap_enabled\":%s,\"sta_eap_identity\":\"%s\","
        "\"sta_eap_username\":\"%s\",\"sta_eap_password\":\"\","
        "\"sta_retry_max\":%u,\"sta_backoff_s\":%u,"
        "\"hostname\":\"%s\",\"sta_mac_custom\":%s,\"sta_mac\":\"%s\","
        "\"ap_mac_custom\":%s,\"ap_mac\":\"%s\",\"port_fwd\":[",
        s_config->sta_eap_enabled ? "true" : "false",
        esc_eap_id, esc_eap_user,
        s_config->sta_retry_max,
        s_config->sta_backoff_s,
        esc_hostname,
        s_config->sta_mac_custom ? "true" : "false",
        sta_mac,
        s_config->ap_mac_custom ? "true" : "false",
        ap_mac);
    httpd_resp_send_chunk(req, buf, len);

    for (int i = 0; i < CFG_PORT_FWD_MAX; i++) {
        esp_ip4_addr_t ip = { .addr = s_config->port_fwd[i].int_ip };
        char entry[128];
        snprintf(entry, sizeof(entry),
            "%s{\"enabled\":%s,\"proto\":%d,\"ext_port\":%d,"
            "\"int_ip\":\"" IPSTR "\",\"int_port\":%d}",
            i > 0 ? "," : "",
            s_config->port_fwd[i].enabled ? "true" : "false",
            s_config->port_fwd[i].proto,
            s_config->port_fwd[i].ext_port,
            IP2STR(&ip),
            s_config->port_fwd[i].int_port);
        httpd_resp_send_chunk(req, entry, strlen(entry));
    }

    len = snprintf(buf, sizeof(buf),
        "],\"log_level_global\":%u,\"log_level_microlink\":%u}",
        s_config->log_level_global,
        s_config->log_level_microlink);
    httpd_resp_send_chunk(req, buf, len);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// Simple JSON string value extractor (no full parser needed for our small payloads)
static bool json_get_string(const char *json, const char *key, char *out, size_t out_size)
{
    char search[48];
    snprintf(search, sizeof(search), "\"%s\":\"", key);
    const char *start = strstr(json, search);
    if (!start) return false;
    start += strlen(search);
    const char *end = strchr(start, '"');
    if (!end) return false;
    size_t len = end - start;
    if (len >= out_size) len = out_size - 1;
    memcpy(out, start, len);
    out[len] = '\0';
    return true;
}

static bool json_get_int(const char *json, const char *key, int *out)
{
    char search[48];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *start = strstr(json, search);
    if (!start) return false;
    start += strlen(search);
    *out = atoi(start);
    return true;
}

static bool json_is_object_like(const char *json)
{
    while (*json == ' ' || *json == '\t' || *json == '\r' || *json == '\n') json++;
    if (*json != '{') return false;

    const char *end = json + strlen(json);
    while (end > json && (end[-1] == ' ' || end[-1] == '\t' ||
           end[-1] == '\r' || end[-1] == '\n')) {
        end--;
    }
    return end > json && end[-1] == '}';
}

static const char *json_find_value(const char *json, const char *key)
{
    char search[48];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *start = strstr(json, search);
    if (!start) return NULL;

    start += strlen(search);
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') start++;
    if (*start != ':') return NULL;
    start++;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') start++;
    return start;
}

static bool json_get_string_strict(const char *json, const char *key,
                                   char *out, size_t out_size, bool *present)
{
    const char *start = json_find_value(json, key);
    if (present) *present = false;
    if (!start) return true;
    if (present) *present = true;

    if (*start != '"') return false;
    start++;
    const char *end = strchr(start, '"');
    if (!end) return false;

    size_t len = end - start;
    if (len >= out_size) return false;
    memcpy(out, start, len);
    out[len] = '\0';
    return true;
}

static bool json_get_bool_strict(const char *json, const char *key, bool *out, bool *present)
{
    const char *start = json_find_value(json, key);
    if (present) *present = false;
    if (!start) return true;
    if (present) *present = true;

    if (strncmp(start, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (strncmp(start, "false", 5) == 0) {
        *out = false;
        return true;
    }
    if (*start == '1') {
        *out = true;
        return true;
    }
    if (*start == '0') {
        *out = false;
        return true;
    }
    return false;
}

static bool json_get_int_strict(const char *json, const char *key, int *out, bool *present)
{
    const char *start = json_find_value(json, key);
    if (present) *present = false;
    if (!start) return true;
    if (present) *present = true;

    char *end = NULL;
    long value = strtol(start, &end, 10);
    if (end == start) return false;
    *out = (int)value;
    return true;
}

static bool tailscale_device_name_valid(const char *value)
{
    if (value[0] == '\0') return false;
    for (size_t i = 0; value[i] != '\0'; i++) {
        char c = value[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') {
            continue;
        }
        return false;
    }
    return true;
}

static bool tailscale_control_host_valid(const char *value)
{
    for (size_t i = 0; value[i] != '\0'; i++) {
        char c = value[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '.' || c == ':') {
            continue;
        }
        return false;
    }
    return true;
}

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

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
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
    if (mac[0] & 0x01) return false;
    return true;
}

static bool parse_custom_mac(const char *value, uint8_t mac[6])
{
    if (strlen(value) != 17) return false;
    for (int i = 0; i < 6; i++) {
        int hi = hex_value(value[i * 3]);
        int lo = hex_value(value[i * 3 + 1]);
        if (hi < 0 || lo < 0) return false;
        mac[i] = (uint8_t)((hi << 4) | lo);
        if (i < 5 && value[i * 3 + 2] != ':') return false;
    }
    return custom_mac_valid(mac);
}

static bool mac_equal(const uint8_t a[6], const uint8_t b[6])
{
    return memcmp(a, b, 6) == 0;
}

static uint32_t cidr_mask_from_prefix(int prefix)
{
    return prefix == 32 ? 0xffffffffU : (0xffffffffU << (32 - prefix));
}

static bool cidr_range_overlaps(uint32_t network, int prefix,
                                uint32_t blocked_network, int blocked_prefix)
{
    uint32_t mask = cidr_mask_from_prefix(prefix);
    uint32_t blocked_mask = cidr_mask_from_prefix(blocked_prefix);
    uint32_t start = network & mask;
    uint32_t end = start | ~mask;
    uint32_t blocked_start = blocked_network & blocked_mask;
    uint32_t blocked_end = blocked_start | ~blocked_mask;

    return start <= blocked_end && blocked_start <= end;
}

static bool parse_ipv4_cidr(const char *value, uint32_t *network_out, int *prefix_out)
{
    if (!value || value[0] == '\0') return false;

    const char *slash = strchr(value, '/');
    if (!slash || strchr(slash + 1, '/')) return false;

    int prefix = 0;
    if (slash[1] == '\0') return false;
    for (const char *p = slash + 1; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') return false;
        prefix = prefix * 10 + (*p - '0');
        if (prefix > 32) return false;
    }
    if (prefix < 1 || prefix > 32) return false;

    uint32_t octets[4] = {0};
    const char *p = value;
    for (int i = 0; i < 4; i++) {
        int digits = 0;
        uint32_t octet = 0;
        while (p < slash && *p >= '0' && *p <= '9') {
            octet = (octet * 10) + (uint32_t)(*p - '0');
            if (octet > 255 || ++digits > 3) return false;
            p++;
        }
        if (digits == 0) return false;
        octets[i] = octet;

        if (i < 3) {
            if (p >= slash || *p != '.') return false;
            p++;
        } else if (p != slash) {
            return false;
        }
    }

    uint32_t addr = (octets[0] << 24) | (octets[1] << 16) |
                    (octets[2] << 8) | octets[3];
    uint32_t mask = cidr_mask_from_prefix(prefix);
    uint32_t network = addr & mask;
    if (addr != network) return false;

    if (network_out) *network_out = network;
    if (prefix_out) *prefix_out = prefix;
    return true;
}

static bool tailscale_cidr4_valid(const char *value)
{
    uint32_t network = 0;
    int prefix = 0;
    if (!parse_ipv4_cidr(value, &network, &prefix)) return false;

    if (cidr_range_overlaps(network, prefix, 0x00000000U, 8)) return false;   /* 0.0.0.0/8 */
    if (cidr_range_overlaps(network, prefix, 0x64400000U, 10)) return false;  /* 100.64.0.0/10 */
    if (cidr_range_overlaps(network, prefix, 0x7f000000U, 8)) return false;   /* 127.0.0.0/8 */
    if (cidr_range_overlaps(network, prefix, 0xa9fe0000U, 16)) return false;  /* 169.254.0.0/16 */
    if (cidr_range_overlaps(network, prefix, 0xe0000000U, 4)) return false;   /* 224.0.0.0/4 */

    return true;
}

static bool scheduler_tz_valid_http(const char *value)
{
    size_t len = strlen(value);
    if (len == 0 || len >= CFG_SCHED_TZ_LEN) return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)value[i];
        if (c < 0x21 || c > 0x7e) return false;
    }
    return true;
}

static bool scheduler_note_valid(const char *value)
{
    for (size_t i = 0; value[i] != '\0'; i++) {
        unsigned char c = (unsigned char)value[i];
        if (c < 0x20 || c > 0x7e) return false;
    }
    return true;
}

static bool scheduler_rule_valid_http(const scheduler_rule_t *rule)
{
    if (!rule->enabled) return true;
    if ((rule->days & 0x7f) == 0) return false;
    if (rule->start_min >= 1440 || rule->end_min > 1440) return false;
    if (rule->start_min >= rule->end_min) return false;
    return true;
}

static esp_err_t api_config_post_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    char buf[2048];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    char val[65];
    int ival;
    bool present = false;
    bool bval = false;
    char sval[CFG_HOSTNAME_LEN];
    uint8_t new_sta_retry_max = s_config->sta_retry_max;
    uint16_t new_sta_backoff_s = s_config->sta_backoff_s;
    char new_hostname[CFG_HOSTNAME_LEN];
    uint8_t new_sta_mac[6];
    uint8_t new_ap_mac[6];
    bool new_sta_mac_custom = s_config->sta_mac_custom;
    bool new_ap_mac_custom = s_config->ap_mac_custom;

    strlcpy(new_hostname, s_config->hostname, sizeof(new_hostname));
    memcpy(new_sta_mac, s_config->sta_mac, sizeof(new_sta_mac));
    memcpy(new_ap_mac, s_config->ap_mac, sizeof(new_ap_mac));

    if (!json_get_int_strict(buf, "sta_retry_max", &ival, &present)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid sta_retry_max");
        return ESP_FAIL;
    }
    if (present) {
        if (ival < CFG_STA_RETRY_MIN || ival > CFG_STA_RETRY_MAX_LIMIT) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "sta_retry_max out of range");
            return ESP_FAIL;
        }
        new_sta_retry_max = (uint8_t)ival;
    }

    if (!json_get_int_strict(buf, "sta_backoff_s", &ival, &present)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid sta_backoff_s");
        return ESP_FAIL;
    }
    if (present) {
        if (ival < CFG_STA_BACKOFF_MIN_S || ival > CFG_STA_BACKOFF_MAX_S) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "sta_backoff_s out of range");
            return ESP_FAIL;
        }
        new_sta_backoff_s = (uint16_t)ival;
    }

    if (!json_get_string_strict(buf, "hostname", sval, sizeof(sval), &present)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid hostname");
        return ESP_FAIL;
    }
    if (present) {
        if (!hostname_valid(sval)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "hostname must be 1-31 chars: A-Z, a-z, 0-9 or hyphen");
            return ESP_FAIL;
        }
        strlcpy(new_hostname, sval, sizeof(new_hostname));
    }

    if (!json_get_bool_strict(buf, "sta_mac_custom", &bval, &present)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid sta_mac_custom");
        return ESP_FAIL;
    }
    if (present) new_sta_mac_custom = bval;

    char mac_sval[18];
    if (!json_get_string_strict(buf, "sta_mac", mac_sval, sizeof(mac_sval), &present)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid sta_mac");
        return ESP_FAIL;
    }
    if (present && new_sta_mac_custom && !parse_custom_mac(mac_sval, new_sta_mac)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "STA MAC must be unicast and not all-zero/all-FF");
        return ESP_FAIL;
    }

    if (!json_get_bool_strict(buf, "ap_mac_custom", &bval, &present)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid ap_mac_custom");
        return ESP_FAIL;
    }
    if (present) new_ap_mac_custom = bval;

    if (!json_get_string_strict(buf, "ap_mac", mac_sval, sizeof(mac_sval), &present)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid ap_mac");
        return ESP_FAIL;
    }
    if (present && new_ap_mac_custom && !parse_custom_mac(mac_sval, new_ap_mac)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "AP MAC must be unicast and not all-zero/all-FF");
        return ESP_FAIL;
    }

    if (new_sta_mac_custom && !custom_mac_valid(new_sta_mac)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "STA custom MAC is enabled but invalid");
        return ESP_FAIL;
    }
    if (new_ap_mac_custom && !custom_mac_valid(new_ap_mac)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "AP custom MAC is enabled but invalid");
        return ESP_FAIL;
    }
    if (new_sta_mac_custom && new_ap_mac_custom && mac_equal(new_sta_mac, new_ap_mac)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "STA and AP custom MAC must differ");
        return ESP_FAIL;
    }

    if (json_get_string(buf, "sta_ssid", val, sizeof(val))) {
        strlcpy(s_config->sta_ssid, val, sizeof(s_config->sta_ssid));
    }
    if (json_get_string(buf, "sta_pass", val, sizeof(val)) && strlen(val) > 0) {
        strlcpy(s_config->sta_pass, val, sizeof(s_config->sta_pass));
    }
    if (json_get_string(buf, "ap_ssid", val, sizeof(val)) && strlen(val) > 0) {
        strlcpy(s_config->ap_ssid, val, sizeof(s_config->ap_ssid));
    }
    if (json_get_string(buf, "ap_pass", val, sizeof(val)) && strlen(val) > 0) {
        strlcpy(s_config->ap_pass, val, sizeof(s_config->ap_pass));
    }
    if (json_get_int(buf, "ap_channel", &ival)) {
        s_config->ap_channel = (uint8_t)ival;
    }
    if (json_get_int(buf, "ap_max_conn", &ival)) {
        s_config->ap_max_conn = (uint8_t)ival;
    }
    if (json_get_int(buf, "ap_hide_ssid", &ival)) {
        s_config->ap_hide_ssid = ival ? true : false;
    }

    s_config->sta_retry_max = new_sta_retry_max;
    s_config->sta_backoff_s = new_sta_backoff_s;
    strlcpy(s_config->hostname, new_hostname, sizeof(s_config->hostname));
    memcpy(s_config->sta_mac, new_sta_mac, sizeof(s_config->sta_mac));
    s_config->sta_mac_custom = new_sta_mac_custom;
    memcpy(s_config->ap_mac, new_ap_mac, sizeof(s_config->ap_mac));
    s_config->ap_mac_custom = new_ap_mac_custom;

    // EAP fields
    int eap_en;
    if (json_get_int(buf, "sta_eap_enabled", &eap_en)) {
        s_config->sta_eap_enabled = eap_en ? true : false;
    }
    if (json_get_string(buf, "sta_eap_identity", val, sizeof(val))) {
        strlcpy(s_config->sta_eap_identity, val, sizeof(s_config->sta_eap_identity));
    }
    if (json_get_string(buf, "sta_eap_username", val, sizeof(val))) {
        strlcpy(s_config->sta_eap_username, val, sizeof(s_config->sta_eap_username));
    }
    if (json_get_string(buf, "sta_eap_password", val, sizeof(val)) && strlen(val) > 0) {
        strlcpy(s_config->sta_eap_password, val, sizeof(s_config->sta_eap_password));
    }

    // Port forwarding rules (flat keys: pf0_enabled .. pf4_int_port)
    for (int i = 0; i < CFG_PORT_FWD_MAX; i++) {
        char key[16];
        int v;
        snprintf(key, sizeof(key), "pf%d_enabled", i);
        if (json_get_int(buf, key, &v)) s_config->port_fwd[i].enabled = v ? true : false;
        snprintf(key, sizeof(key), "pf%d_proto", i);
        if (json_get_int(buf, key, &v)) s_config->port_fwd[i].proto = (uint8_t)v;
        snprintf(key, sizeof(key), "pf%d_ext_port", i);
        if (json_get_int(buf, key, &v)) s_config->port_fwd[i].ext_port = (uint16_t)v;
        snprintf(key, sizeof(key), "pf%d_int_port", i);
        if (json_get_int(buf, key, &v)) s_config->port_fwd[i].int_port = (uint16_t)v;
        char ip_str[16];
        snprintf(key, sizeof(key), "pf%d_int_ip", i);
        if (json_get_string(buf, key, ip_str, sizeof(ip_str))) {
            uint32_t parsed = esp_ip4addr_aton(ip_str);
            if (parsed != IPADDR_NONE) {
                s_config->port_fwd[i].int_ip = parsed;
            }
        }
    }

    /* Runtime log levels — accept and clamp to 0..5. */
    if (json_get_int(buf, "log_level_global", &ival)) {
        if (ival < 0) ival = 0;
        if (ival > CFG_LOG_LEVEL_VERBOSE) ival = CFG_LOG_LEVEL_VERBOSE;
        s_config->log_level_global = (uint8_t)ival;
    }
    if (json_get_int(buf, "log_level_microlink", &ival)) {
        if (ival < 0) ival = 0;
        if (ival > CFG_LOG_LEVEL_VERBOSE) ival = CFG_LOG_LEVEL_VERBOSE;
        s_config->log_level_microlink = (uint8_t)ival;
    }

    esp_err_t save_ret = config_storage_save(s_config);
    if (save_ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Config save failed");
        return ESP_FAIL;
    }

    /* Apply log levels immediately, no reboot needed. */
    config_storage_apply_log_levels(s_config);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Config saved. WiFi is restarting...\"}");

    /* Reconfiguring WiFi can drop the HTTP transport, especially when the AP
     * or STA MAC changes. Give the response a short window to leave first. */
    vTaskDelay(pdMS_TO_TICKS(500));
    wifi_manager_reconfigure(s_config);
    return ESP_OK;
}

static esp_err_t api_loglevel_post_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;

    char buf[128];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    int ival;
    bool changed = false;
    if (json_get_int(buf, "log_level_global", &ival)) {
        if (ival < 0) ival = 0;
        if (ival > CFG_LOG_LEVEL_VERBOSE) ival = CFG_LOG_LEVEL_VERBOSE;
        s_config->log_level_global = (uint8_t)ival;
        changed = true;
    }
    if (json_get_int(buf, "log_level_microlink", &ival)) {
        if (ival < 0) ival = 0;
        if (ival > CFG_LOG_LEVEL_VERBOSE) ival = CFG_LOG_LEVEL_VERBOSE;
        s_config->log_level_microlink = (uint8_t)ival;
        changed = true;
    }
    if (!changed) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No log_level_* field");
        return ESP_FAIL;
    }

    config_storage_save(s_config);
    config_storage_apply_log_levels(s_config);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Log levels applied\"}");
    return ESP_OK;
}

static esp_err_t api_scheduler_status_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;

    scheduler_status_t status;
    scheduler_manager_get_status(&status);

    char esc_tz[CFG_SCHED_TZ_LEN * 2];
    char esc_local[SCHED_TIME_STR_LEN * 2];
    char esc_next[SCHED_TIME_STR_LEN * 2];
    char esc_reason[SCHED_REASON_LEN * 2];
    json_escape(esc_tz, sizeof(esc_tz), status.timezone);
    json_escape(esc_local, sizeof(esc_local), status.local_time);
    json_escape(esc_next, sizeof(esc_next), status.next_change_local);
    json_escape(esc_reason, sizeof(esc_reason), status.reason);

    char buf[896];
    snprintf(buf, sizeof(buf),
        "{\"time_valid\":%s,\"ntp_synced\":%s,\"timezone\":\"%s\","
        "\"local_time\":\"%s\",\"mode\":%u,\"ap_effective\":%s,"
        "\"ap_desired\":%s,\"sta_effective\":%s,\"sta_desired\":%s,"
        "\"tailscale_effective\":%s,\"tailscale_desired\":%s,"
        "\"safety_hold\":%s,\"active_rule\":%d,"
        "\"next_rule\":%d,\"next_change_s\":%lu,"
        "\"next_change_local\":\"%s\",\"reason\":\"%s\"}",
        status.time_valid ? "true" : "false",
        status.ntp_synced ? "true" : "false",
        esc_tz, esc_local, (unsigned)status.mode,
        status.ap_effective ? "true" : "false",
        status.ap_desired ? "true" : "false",
        status.sta_effective ? "true" : "false",
        status.sta_desired ? "true" : "false",
        status.tailscale_effective ? "true" : "false",
        status.tailscale_desired ? "true" : "false",
        status.safety_hold ? "true" : "false",
        status.active_rule, status.next_rule,
        (unsigned long)status.next_change_s,
        esc_next, esc_reason);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t api_scheduler_config_get_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;

    char esc_tz[CFG_SCHED_TZ_LEN * 2];
    json_escape(esc_tz, sizeof(esc_tz), s_config->sched_tz);

    httpd_resp_set_type(req, "application/json");
    char buf[384];
    int len = snprintf(buf, sizeof(buf),
        "{\"timezone\":\"%s\",\"mode\":%u,\"rules\":[",
        esc_tz, (unsigned)s_config->sched_mode);
    httpd_resp_send_chunk(req, buf, len);

    for (int i = 0; i < CFG_SCHED_RULES_MAX; i++) {
        const scheduler_rule_t *rule = &s_config->sched_rules[i];
        char esc_note[CFG_SCHED_NOTE_LEN * 2];
        json_escape(esc_note, sizeof(esc_note), rule->note);
        len = snprintf(buf, sizeof(buf),
            "%s{\"enabled\":%s,\"days\":%u,\"start_min\":%u,"
            "\"end_min\":%u,\"ap_enabled\":%s,\"sta_enabled\":%s,"
            "\"tailscale_enabled\":%s,\"note\":\"%s\"}",
            i ? "," : "",
            rule->enabled ? "true" : "false",
            rule->days,
            rule->start_min,
            rule->end_min,
            rule->ap_enabled ? "true" : "false",
            rule->sta_enabled ? "true" : "false",
            rule->tailscale_enabled ? "true" : "false",
            esc_note);
        httpd_resp_send_chunk(req, buf, len);
    }
    httpd_resp_send_chunk(req, "]}", 2);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t api_scheduler_config_post_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;

    char buf[3072];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    char sval[CFG_SCHED_TZ_LEN];
    bool present = false;
    bool bval = false;
    int ival = 0;
    scheduler_mode_t new_mode = s_config->sched_mode;
    char new_tz[CFG_SCHED_TZ_LEN];
    scheduler_rule_t new_rules[CFG_SCHED_RULES_MAX];
    strlcpy(new_tz, s_config->sched_tz, sizeof(new_tz));
    memcpy(new_rules, s_config->sched_rules, sizeof(new_rules));

    if (!json_get_string_strict(buf, "timezone", sval, sizeof(sval), &present)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid timezone");
        return ESP_FAIL;
    }
    if (present) {
        if (!scheduler_tz_valid_http(sval)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "timezone must be printable POSIX TZ");
            return ESP_FAIL;
        }
        strlcpy(new_tz, sval, sizeof(new_tz));
    }

    if (!json_get_int_strict(buf, "mode", &ival, &present)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid mode");
        return ESP_FAIL;
    }
    if (present) {
        if (ival < SCHED_MODE_ALWAYS_ON || ival > SCHED_MODE_MANUAL_OFF) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mode out of range");
            return ESP_FAIL;
        }
        new_mode = (scheduler_mode_t)ival;
    }

    for (int i = 0; i < CFG_SCHED_RULES_MAX; i++) {
        char key[24];
        scheduler_rule_t *rule = &new_rules[i];

        snprintf(key, sizeof(key), "rule%d_enabled", i);
        if (!json_get_bool_strict(buf, key, &bval, &present)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid rule enabled");
            return ESP_FAIL;
        }
        if (present) rule->enabled = bval;

        snprintf(key, sizeof(key), "rule%d_days", i);
        if (!json_get_int_strict(buf, key, &ival, &present)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid rule days");
            return ESP_FAIL;
        }
        if (present) {
            if (ival < 0 || ival > 0x7f) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "rule days out of range");
                return ESP_FAIL;
            }
            rule->days = (uint8_t)ival;
        }

        snprintf(key, sizeof(key), "rule%d_start_min", i);
        if (!json_get_int_strict(buf, key, &ival, &present)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid rule start");
            return ESP_FAIL;
        }
        if (present) {
            if (ival < 0 || ival >= 1440) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "rule start out of range");
                return ESP_FAIL;
            }
            rule->start_min = (uint16_t)ival;
        }

        snprintf(key, sizeof(key), "rule%d_end_min", i);
        if (!json_get_int_strict(buf, key, &ival, &present)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid rule end");
            return ESP_FAIL;
        }
        if (present) {
            if (ival < 0 || ival > 1440) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "rule end out of range");
                return ESP_FAIL;
            }
            rule->end_min = (uint16_t)ival;
        }

        snprintf(key, sizeof(key), "rule%d_ap_enabled", i);
        if (!json_get_bool_strict(buf, key, &bval, &present)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid rule AP state");
            return ESP_FAIL;
        }
        if (present) rule->ap_enabled = bval;

        snprintf(key, sizeof(key), "rule%d_sta_enabled", i);
        if (!json_get_bool_strict(buf, key, &bval, &present)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid rule STA state");
            return ESP_FAIL;
        }
        if (present) rule->sta_enabled = bval;

        snprintf(key, sizeof(key), "rule%d_tailscale_enabled", i);
        if (!json_get_bool_strict(buf, key, &bval, &present)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid rule Tailscale state");
            return ESP_FAIL;
        }
        if (present) rule->tailscale_enabled = bval;

        char note[CFG_SCHED_NOTE_LEN];
        snprintf(key, sizeof(key), "rule%d_note", i);
        if (!json_get_string_strict(buf, key, note, sizeof(note), &present)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid rule note");
            return ESP_FAIL;
        }
        if (present) {
            if (!scheduler_note_valid(note)) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "rule note must be printable ASCII");
                return ESP_FAIL;
            }
            strlcpy(rule->note, note, sizeof(rule->note));
        }

        if (!scheduler_rule_valid_http(rule)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "enabled rules need days and start < end");
            return ESP_FAIL;
        }
    }

    if (new_mode == SCHED_MODE_MANUAL_OFF) {
        wifi_status_t wifi = {0};
        wifi_manager_get_status(&wifi);
        if (!wifi.sta_connected) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Manual AP off requires active STA management path");
            return ESP_FAIL;
        }
    }

    strlcpy(s_config->sched_tz, new_tz, sizeof(s_config->sched_tz));
    s_config->sched_mode = new_mode;
    memcpy(s_config->sched_rules, new_rules, sizeof(s_config->sched_rules));

    esp_err_t save_ret = config_storage_save(s_config);
    if (save_ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Scheduler save failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Scheduler saved\"}");
    vTaskDelay(pdMS_TO_TICKS(200));
    scheduler_manager_apply_config(s_config);
    return ESP_OK;
}

static esp_err_t api_scheduler_sync_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    scheduler_manager_sync_now();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Time sync requested\"}");
    return ESP_OK;
}

static esp_err_t api_tailscale_status_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;

    tailscale_status_t status;
    tailscale_manager_get_status(&status);

    httpd_resp_set_type(req, "application/json");
    if (!status.available) {
        httpd_resp_sendstr(req, "{\"available\":false,\"reason\":\"not_compiled\"}");
        return ESP_OK;
    }

    char esc_vpn_ip[TAILSCALE_VPN_IP_LEN * 2];
    char esc_route[CFG_TS_CTRL_HOST_LEN * 2];
    char esc_route_state[TAILSCALE_ROUTE_STATE_LEN * 2];
    json_escape(esc_vpn_ip, sizeof(esc_vpn_ip), status.vpn_ip);
    json_escape(esc_route, sizeof(esc_route), status.advertised_cidr);
    json_escape(esc_route_state, sizeof(esc_route_state), status.subnet_route_state);

    char buf[672];
    snprintf(buf, sizeof(buf),
        "{\"available\":true,\"enabled\":%s,\"scheduler_enabled\":%s,\"configured\":%s,"
        "\"started\":%s,\"connected\":%s,\"state\":\"%s\","
        "\"vpn_ip\":\"%s\",\"peer_count\":%u,\"last_error\":%d,"
        "\"subnet_route_advertised\":%s,\"subnet_route_active\":%s,"
        "\"advertised_cidr\":\"%s\",\"subnet_route_state\":\"%s\","
        "\"heap_internal_free\":%lu,\"heap_psram_free\":%lu}",
        status.enabled ? "true" : "false",
        status.scheduler_enabled ? "true" : "false",
        status.configured ? "true" : "false",
        status.started ? "true" : "false",
        status.connected ? "true" : "false",
        tailscale_manager_state_to_string(status.state),
        esc_vpn_ip,
        status.peer_count,
        (int)status.last_error,
        status.subnet_route_advertised ? "true" : "false",
        status.subnet_route_active ? "true" : "false",
        esc_route,
        esc_route_state,
        (unsigned long)status.heap_internal_free,
        (unsigned long)status.heap_psram_free);
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t api_tailscale_config_get_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;

    char esc_name[CFG_TS_NAME_LEN * 2];
    char esc_ctrl[CFG_TS_CTRL_HOST_LEN * 2];
    char esc_cidr[CFG_TS_CTRL_HOST_LEN * 2];
    json_escape(esc_name, sizeof(esc_name), s_config->tailscale.device_name);
    json_escape(esc_ctrl, sizeof(esc_ctrl), s_config->tailscale.control_host);
    json_escape(esc_cidr, sizeof(esc_cidr), s_config->tailscale.advertise_cidr);

    char buf[768];
    snprintf(buf, sizeof(buf),
        "{\"enabled\":%s,\"auth_key_set\":%s,"
        "\"device_name\":\"%s\",\"control_host\":\"%s\","
        "\"enable_derp\":%s,\"enable_disco\":%s,\"enable_stun\":%s,"
        "\"max_peers\":%u,\"expose_web_ui\":%s,"
        "\"expose_lan\":%s,\"net_mode\":%u,\"advertise_cidr\":\"%s\"}",
        s_config->tailscale.enabled ? "true" : "false",
        s_config->tailscale.auth_key[0] ? "true" : "false",
        esc_name,
        esc_ctrl,
        s_config->tailscale.enable_derp ? "true" : "false",
        s_config->tailscale.enable_disco ? "true" : "false",
        s_config->tailscale.enable_stun ? "true" : "false",
        s_config->tailscale.max_peers,
        s_config->tailscale.expose_web_ui ? "true" : "false",
        s_config->tailscale.expose_lan ? "true" : "false",
        (unsigned)s_config->tailscale.net_mode,
        esc_cidr);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t api_tailscale_config_post_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;

    char buf[768];
    if (req->content_len <= 0 || req->content_len >= (int)sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid payload size");
        return ESP_FAIL;
    }

    int total = 0;
    while (total < req->content_len) {
        int received = httpd_req_recv(req, buf + total, req->content_len - total);
        if (received <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Receive failed");
            return ESP_FAIL;
        }
        total += received;
    }
    buf[total] = '\0';

    if (!json_is_object_like(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    bool present;
    bool bval;
    int ival;
    char cidr_buf[CFG_TS_CTRL_HOST_LEN];
    char sval[CFG_TS_AUTH_KEY_LEN];
    network_mode_t old_net_mode = s_config->tailscale.net_mode;

    if (!json_get_bool_strict(buf, "enabled", &bval, &present)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid enabled");
        return ESP_FAIL;
    }
    if (present) s_config->tailscale.enabled = bval;

    if (!json_get_string_strict(buf, "auth_key", sval, sizeof(sval), &present)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid auth_key");
        return ESP_FAIL;
    }
    if (present) {
        strlcpy(s_config->tailscale.auth_key, sval, sizeof(s_config->tailscale.auth_key));
    }

    if (!json_get_string_strict(buf, "device_name", sval, CFG_TS_NAME_LEN, &present)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid device_name");
        return ESP_FAIL;
    }
    if (present) {
        if (!tailscale_device_name_valid(sval)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid device_name");
            return ESP_FAIL;
        }
        strlcpy(s_config->tailscale.device_name, sval, sizeof(s_config->tailscale.device_name));
    }

    if (!json_get_string_strict(buf, "control_host", sval, CFG_TS_CTRL_HOST_LEN, &present)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid control_host");
        return ESP_FAIL;
    }
    if (present) {
        if (!tailscale_control_host_valid(sval)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid control_host");
            return ESP_FAIL;
        }
        strlcpy(s_config->tailscale.control_host, sval, sizeof(s_config->tailscale.control_host));
    }

    if (!json_get_bool_strict(buf, "enable_derp", &bval, &present)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid enable_derp");
        return ESP_FAIL;
    }
    if (present) s_config->tailscale.enable_derp = bval;

    if (!json_get_bool_strict(buf, "enable_disco", &bval, &present)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid enable_disco");
        return ESP_FAIL;
    }
    if (present) s_config->tailscale.enable_disco = bval;

    if (!json_get_bool_strict(buf, "enable_stun", &bval, &present)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid enable_stun");
        return ESP_FAIL;
    }
    if (present) s_config->tailscale.enable_stun = bval;

    if (!json_get_int_strict(buf, "max_peers", &ival, &present)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid max_peers");
        return ESP_FAIL;
    }
    if (present) {
        if (ival < CFG_TS_MAX_PEERS_MIN || ival > CFG_TS_MAX_PEERS_MAX) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid max_peers");
            return ESP_FAIL;
        }
        s_config->tailscale.max_peers = (uint8_t)ival;
    }

    if (!json_get_bool_strict(buf, "expose_web_ui", &bval, &present)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid expose_web_ui");
        return ESP_FAIL;
    }
    if (present) s_config->tailscale.expose_web_ui = bval;

    if (!json_get_int_strict(buf, "net_mode", &ival, &present)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid net_mode");
        return ESP_FAIL;
    }
    if (present) {
        if (ival != NET_MODE_REPEATER && ival != NET_MODE_TS_GATEWAY) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid net_mode");
            return ESP_FAIL;
        }
        s_config->tailscale.net_mode = (network_mode_t)ival;
    }

    if (!json_get_bool_strict(buf, "expose_lan", &bval, &present)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid expose_lan");
        return ESP_FAIL;
    }
    if (present) s_config->tailscale.expose_lan = bval;

    if (!json_get_string_strict(buf, "advertise_cidr", cidr_buf, sizeof(cidr_buf), &present)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid advertise_cidr");
        return ESP_FAIL;
    }
    if (present) {
        if (cidr_buf[0] != '\0' && !tailscale_cidr4_valid(cidr_buf)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid advertise_cidr");
            return ESP_FAIL;
        }
        strlcpy(s_config->tailscale.advertise_cidr, cidr_buf, sizeof(s_config->tailscale.advertise_cidr));
    }

    /* Product rule: route advertising means Gateway/SNAT. Keeping route-only
     * hidden avoids implying that advertised routes work without either SNAT
     * or router changes. */
    s_config->tailscale.net_mode = s_config->tailscale.expose_lan ?
                                   NET_MODE_TS_GATEWAY : NET_MODE_REPEATER;
    if (s_config->tailscale.expose_lan &&
        !tailscale_cidr4_valid(s_config->tailscale.advertise_cidr)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid advertise_cidr");
        return ESP_FAIL;
    }

    esp_err_t ret = config_storage_save(s_config);
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Config save failed");
        return ESP_FAIL;
    }

    /* Send HTTP response BEFORE applying config — tailscale_manager may
     * call esp_restart() if MicroLink is already running, which would
     * kill the HTTP connection before the client sees the OK. */
    httpd_resp_set_type(req, "application/json");
    if (old_net_mode != s_config->tailscale.net_mode) {
        httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Network mode changed. Rebooting to apply NAT mode...\"}");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
        return ESP_OK;
    }

    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Tailscale config saved\"}");

    tailscale_manager_apply_config(s_config);
    return ESP_OK;
}

static esp_err_t api_clients_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    wifi_sta_list_t sta_list;
    wifi_sta_mac_ip_list_t ip_list;

    esp_err_t ret = esp_wifi_ap_get_sta_list(&sta_list);
    if (ret != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "[]");
        return ESP_OK;
    }

    esp_wifi_ap_get_sta_list_with_ip(&sta_list, &ip_list);

    // Each client entry ~80 bytes, max 10 clients
    char buf[1024];
    int pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos, "[");
    for (int i = 0; i < ip_list.num; i++) {
        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), MACSTR, MAC2STR(ip_list.sta[i].mac));
        char ip_str[16];
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_list.sta[i].ip));
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"mac\":\"%s\",\"ip\":\"%s\"}",
            i > 0 ? "," : "", mac_str, ip_str);
    }
    snprintf(buf + pos, sizeof(buf) - pos, "]");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t api_ping_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    char buf[128];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    char target[64];
    if (!json_get_string(buf, "target", target, sizeof(target)) || strlen(target) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing target");
        return ESP_FAIL;
    }

    ping_result_t result;
    esp_err_t ret = wifi_manager_ping(target, &result);

    char resp[256];
    if (ret == ESP_OK && result.success) {
        char ip_str[16];
        esp_ip4_addr_t ip = { .addr = result.addr };
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip));
        snprintf(resp, sizeof(resp),
            "{\"success\":true,\"ip\":\"%s\",\"time_ms\":%lu,\"target\":\"%s\"}",
            ip_str, (unsigned long)result.elapsed_ms, target);
    } else {
        const char *reason = "timeout";
        if (ret == ESP_ERR_NOT_FOUND) reason = "dns_failed";
        snprintf(resp, sizeof(resp),
            "{\"success\":false,\"reason\":\"%s\",\"target\":\"%s\"}",
            reason, target);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t api_logs_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;

    /* Buffer can be up to 32 KB; prefer PSRAM to avoid DRAM pressure. */
    char *buf = heap_caps_malloc(LOG_BUFFER_SIZE + 1,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = malloc(LOG_BUFFER_SIZE + 1);
    }
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    size_t len = log_buffer_read(buf, LOG_BUFFER_SIZE + 1);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, buf, len);
    free(buf);
    return ESP_OK;
}

static esp_err_t api_restart_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Restarting...\"}");

    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t api_auth_change_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;

    char buf[256];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    char new_user[CFG_USER_LEN];
    char new_pass[CFG_PASS_LEN];

    if (!json_get_string(buf, "new_user", new_user, sizeof(new_user)) ||
        !json_get_string(buf, "new_pass", new_pass, sizeof(new_pass))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing new_user or new_pass");
        return ESP_FAIL;
    }

    if (strlen(new_user) == 0 || strlen(new_pass) < 4) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"User required, password min 4 chars\"}");
        return ESP_OK;
    }

    strlcpy(s_config->web_user, new_user, sizeof(s_config->web_user));
    strlcpy(s_config->web_pass, new_pass, sizeof(s_config->web_pass));
    config_storage_save(s_config);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Credentials updated\"}");
    return ESP_OK;
}

static esp_err_t api_auth_check_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"authenticated\":true}");
    return ESP_OK;
}

static esp_err_t api_ota_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition found");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA: writing to partition '%s' at offset 0x%lx, size %lu",
             update_partition->label, (unsigned long)update_partition->address,
             (unsigned long)update_partition->size);

    esp_ota_handle_t ota_handle;
    esp_err_t ret = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(ret));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }

    char *buf = malloc(4096);
    if (!buf) {
        esp_ota_abort(ota_handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    int total_received = 0;
    int content_len = req->content_len;
    ESP_LOGI(TAG, "OTA: receiving %d bytes", content_len);

    while (total_received < content_len) {
        int received = httpd_req_recv(req, buf, 4096);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
            ESP_LOGE(TAG, "OTA: receive error at %d/%d", total_received, content_len);
            free(buf);
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
            return ESP_FAIL;
        }

        ret = esp_ota_write(ota_handle, buf, received);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "OTA: write failed: %s", esp_err_to_name(ret));
            free(buf);
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA write failed");
            return ESP_FAIL;
        }

        total_received += received;
    }

    free(buf);
    ESP_LOGI(TAG, "OTA: received %d bytes total", total_received);

    ret = esp_ota_end(ota_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OTA: validation failed: %s", esp_err_to_name(ret));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA validation failed");
        return ESP_FAIL;
    }

    ret = esp_ota_set_boot_partition(update_partition);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OTA: set boot partition failed: %s", esp_err_to_name(ret));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Set boot partition failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA: success, rebooting...");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"OTA successful, rebooting...\"}");

    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t api_factory_reset_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;

    ESP_LOGW(TAG, "Factory reset requested!");
    config_storage_erase();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Factory reset done, rebooting...\"}");

    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t captive_redirect_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

esp_err_t web_server_start(repeater_config_t *config)
{
    s_config = config;

    httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();
    http_config.max_uri_handlers = 32;
    http_config.uri_match_fn = httpd_uri_match_wildcard;
    http_config.lru_purge_enable = true;
    http_config.stack_size = 8192;
    http_config.recv_wait_timeout = 30;

    esp_err_t ret = httpd_start(&s_server, &http_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        return ret;
    }

    httpd_uri_t uri_index    = { .uri = "/",            .method = HTTP_GET,  .handler = index_handler };
    httpd_uri_t uri_css      = { .uri = "/styles.css",  .method = HTTP_GET,  .handler = css_handler };
    httpd_uri_t uri_js       = { .uri = "/app.js",      .method = HTTP_GET,  .handler = js_handler };
    httpd_uri_t uri_status   = { .uri = "/api/status",  .method = HTTP_GET,  .handler = api_status_handler };
    httpd_uri_t uri_wifi_state = { .uri = "/api/wifi/state", .method = HTTP_GET,  .handler = api_wifi_state_handler };
    httpd_uri_t uri_wifi_pause = { .uri = "/api/wifi/pause", .method = HTTP_POST, .handler = api_wifi_pause_handler };
    httpd_uri_t uri_wifi_resume = { .uri = "/api/wifi/resume", .method = HTTP_POST, .handler = api_wifi_resume_handler };
    httpd_uri_t uri_scan     = { .uri = "/api/scan",    .method = HTTP_GET,  .handler = api_scan_handler };
    httpd_uri_t uri_cfg_get  = { .uri = "/api/config",  .method = HTTP_GET,  .handler = api_config_get_handler };
    httpd_uri_t uri_cfg_post = { .uri = "/api/config",  .method = HTTP_POST, .handler = api_config_post_handler };
    httpd_uri_t uri_ts_status = { .uri = "/api/tailscale/status", .method = HTTP_GET,  .handler = api_tailscale_status_handler };
    httpd_uri_t uri_ts_cfg_get = { .uri = "/api/tailscale/config", .method = HTTP_GET,  .handler = api_tailscale_config_get_handler };
    httpd_uri_t uri_ts_cfg_post = { .uri = "/api/tailscale/config", .method = HTTP_POST, .handler = api_tailscale_config_post_handler };
    httpd_uri_t uri_clients  = { .uri = "/api/clients", .method = HTTP_GET,  .handler = api_clients_handler };
    httpd_uri_t uri_ping     = { .uri = "/api/ping",        .method = HTTP_POST, .handler = api_ping_handler };
    httpd_uri_t uri_restart  = { .uri = "/api/restart",     .method = HTTP_POST, .handler = api_restart_handler };
    httpd_uri_t uri_auth_chg = { .uri = "/api/auth/change", .method = HTTP_POST, .handler = api_auth_change_handler };
    httpd_uri_t uri_auth_chk = { .uri = "/api/auth/check",  .method = HTTP_GET,  .handler = api_auth_check_handler };
    httpd_uri_t uri_ota      = { .uri = "/api/ota",           .method = HTTP_POST, .handler = api_ota_handler };
    httpd_uri_t uri_freset   = { .uri = "/api/factory-reset", .method = HTTP_POST, .handler = api_factory_reset_handler };
    httpd_uri_t uri_logs     = { .uri = "/api/logs",          .method = HTTP_GET,  .handler = api_logs_handler };
    httpd_uri_t uri_loglevel = { .uri = "/api/loglevel",      .method = HTTP_POST, .handler = api_loglevel_post_handler };
    httpd_uri_t uri_sched_status = { .uri = "/api/scheduler/status", .method = HTTP_GET, .handler = api_scheduler_status_handler };
    httpd_uri_t uri_sched_cfg_get = { .uri = "/api/scheduler/config", .method = HTTP_GET, .handler = api_scheduler_config_get_handler };
    httpd_uri_t uri_sched_cfg_post = { .uri = "/api/scheduler/config", .method = HTTP_POST, .handler = api_scheduler_config_post_handler };
    httpd_uri_t uri_sched_sync = { .uri = "/api/scheduler/sync", .method = HTTP_POST, .handler = api_scheduler_sync_handler };
    httpd_uri_t uri_catchall = { .uri = "/*",               .method = HTTP_GET,  .handler = captive_redirect_handler };

    httpd_register_uri_handler(s_server, &uri_index);
    httpd_register_uri_handler(s_server, &uri_css);
    httpd_register_uri_handler(s_server, &uri_js);
    httpd_register_uri_handler(s_server, &uri_status);
    httpd_register_uri_handler(s_server, &uri_wifi_state);
    httpd_register_uri_handler(s_server, &uri_wifi_pause);
    httpd_register_uri_handler(s_server, &uri_wifi_resume);
    httpd_register_uri_handler(s_server, &uri_scan);
    httpd_register_uri_handler(s_server, &uri_cfg_get);
    httpd_register_uri_handler(s_server, &uri_cfg_post);
    httpd_register_uri_handler(s_server, &uri_ts_status);
    httpd_register_uri_handler(s_server, &uri_ts_cfg_get);
    httpd_register_uri_handler(s_server, &uri_ts_cfg_post);
    httpd_register_uri_handler(s_server, &uri_clients);
    httpd_register_uri_handler(s_server, &uri_ping);
    httpd_register_uri_handler(s_server, &uri_restart);
    httpd_register_uri_handler(s_server, &uri_auth_chg);
    httpd_register_uri_handler(s_server, &uri_auth_chk);
    httpd_register_uri_handler(s_server, &uri_ota);
    httpd_register_uri_handler(s_server, &uri_freset);
    httpd_register_uri_handler(s_server, &uri_logs);
    httpd_register_uri_handler(s_server, &uri_loglevel);
    httpd_register_uri_handler(s_server, &uri_sched_status);
    httpd_register_uri_handler(s_server, &uri_sched_cfg_get);
    httpd_register_uri_handler(s_server, &uri_sched_cfg_post);
    httpd_register_uri_handler(s_server, &uri_sched_sync);
    httpd_register_uri_handler(s_server, &uri_catchall);

    ESP_LOGI(TAG, "HTTP server started on port %d", http_config.server_port);
    return ESP_OK;
}

void web_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "HTTP server stopped");
    }
}
