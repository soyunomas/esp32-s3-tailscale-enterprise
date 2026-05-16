#include <string.h>
#include <stdlib.h>
#include "wifi_manager.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_eap_client.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "lwip/lwip_napt.h"
#include "lwip/inet.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "ping/ping_sock.h"
#include "tailscale_manager.h"
#include "freertos/task.h"
#include "freertos/timers.h"

static const char *TAG = "wifi_manager";

/* Forward declarations */
static void apply_port_forwarding(void);
static void start_proxy_listeners(void);
static void stop_proxy_listeners(void);

/* TCP Proxy types — defined early so functions below can use them */
#define PROXY_BUF_SIZE 1460
typedef struct {
    struct sockaddr_in dest;
    int listen_fd;
    TaskHandle_t task;
} proxy_listener_t;
static proxy_listener_t s_proxy_listeners[CFG_PORT_FWD_MAX] = {0};
static bool s_proxy_running = false;

#define DHCPS_OFFER_DNS 0x02

static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
static repeater_config_t *s_config = NULL;
static wifi_status_t s_status = {0};
static bool s_wifi_started = false;
static bool s_reconfiguring = false;
static bool s_ap_enabled = true;
static bool s_sta_paused = false;
static bool s_sta_scheduler_enabled = true;
static bool s_sta_recovery = false;
static bool s_scanning = false;
static uint8_t s_retry_count = 0;
static TickType_t s_next_reconnect_tick = 0;
static TimerHandle_t s_reconnect_timer = NULL;
static bool s_subnet_routing = false;
static SemaphoreHandle_t s_scan_done = NULL;
static dns_server_handle_t s_dns_handle = NULL;

typedef enum {
    NAPT_IF_NONE = 0,
    NAPT_IF_AP,
} napt_if_t;

static napt_if_t s_napt_if = NAPT_IF_NONE;

#define WIFI_RECOVERY_COOLDOWN_S 60U

static uint32_t reconnect_backoff_s(void);
static void schedule_reconnect(uint32_t delay_s);

static void apply_netif_hostname(void)
{
    if (!s_config) return;

    if (s_sta_netif) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_set_hostname(s_sta_netif, s_config->hostname));
    }
    if (s_ap_netif) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_set_hostname(s_ap_netif, s_config->hostname));
    }
    ESP_LOGI(TAG, "Hostname applied: %s", s_config->hostname);
}

static esp_err_t apply_custom_macs(void)
{
    esp_err_t ret;
    uint8_t mac[6];

    if (!s_config) return ESP_ERR_INVALID_STATE;

    if (s_config->sta_mac_custom) {
        ret = esp_wifi_set_mac(WIFI_IF_STA, s_config->sta_mac);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set custom STA MAC: %s", esp_err_to_name(ret));
            return ret;
        }
        ESP_LOGI(TAG, "Custom STA MAC applied: " MACSTR, MAC2STR(s_config->sta_mac));
    } else {
        ret = esp_read_mac(mac, ESP_MAC_WIFI_STA);
        if (ret == ESP_OK) {
            ret = esp_wifi_set_mac(WIFI_IF_STA, mac);
        }
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to restore default STA MAC: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    if (s_config->ap_mac_custom) {
        ret = esp_wifi_set_mac(WIFI_IF_AP, s_config->ap_mac);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set custom AP MAC: %s", esp_err_to_name(ret));
            return ret;
        }
        ESP_LOGI(TAG, "Custom AP MAC applied: " MACSTR, MAC2STR(s_config->ap_mac));
    } else {
        ret = esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
        if (ret == ESP_OK) {
            ret = esp_wifi_set_mac(WIFI_IF_AP, mac);
        }
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to restore default AP MAC: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    return ESP_OK;
}

static void stop_reconnect_timer(void)
{
    if (s_reconnect_timer) {
        xTimerStop(s_reconnect_timer, 0);
    }
    s_next_reconnect_tick = 0;
}

static void reconnect_timer_cb(TimerHandle_t timer)
{
    (void)timer;
    s_next_reconnect_tick = 0;

    if (!s_wifi_started || s_reconfiguring || s_scanning ||
        s_sta_paused || s_status.sta_connected ||
        !s_config || s_config->sta_ssid[0] == '\0') {
        return;
    }

    ESP_LOGI(TAG, "STA reconnect attempt: retry=%u recovery=%d",
             s_retry_count, s_sta_recovery ? 1 : 0);
    esp_err_t ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(ret));
        schedule_reconnect(s_sta_recovery ? WIFI_RECOVERY_COOLDOWN_S : reconnect_backoff_s());
    }
}

static uint32_t reconnect_backoff_s(void)
{
    uint32_t max_s = s_config ? s_config->sta_backoff_s : CFG_STA_BACKOFF_DEFAULT_S;
    if (max_s < 1) max_s = 1;

    uint32_t delay_s = 2;
    uint8_t shift = s_retry_count > 1 ? (uint8_t)(s_retry_count - 1) : 0;
    if (shift > 6) shift = 6;
    delay_s <<= shift;
    if (delay_s > max_s) delay_s = max_s;
    return delay_s;
}

static void schedule_reconnect(uint32_t delay_s)
{
    if (!s_reconnect_timer || !s_wifi_started || s_reconfiguring ||
        s_scanning || s_sta_paused || s_status.sta_connected ||
        !s_config || s_config->sta_ssid[0] == '\0') {
        s_next_reconnect_tick = 0;
        return;
    }

    if (delay_s == 0) delay_s = 1;
    TickType_t ticks = pdMS_TO_TICKS(delay_s * 1000U);
    if (ticks == 0) ticks = 1;

    xTimerStop(s_reconnect_timer, 0);
    xTimerChangePeriod(s_reconnect_timer, ticks, 0);
    s_next_reconnect_tick = xTaskGetTickCount() + ticks;
    ESP_LOGI(TAG, "STA reconnect scheduled in %lu s", (unsigned long)delay_s);
}

static void restart_captive_dns(void)
{
    if (s_dns_handle == NULL) {
        dns_server_config_t dns_config = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
        s_dns_handle = start_dns_server(&dns_config);
        ESP_LOGI(TAG, "Captive portal DNS restarted");
    }
}

void wifi_manager_set_dns_handle(dns_server_handle_t handle)
{
    s_dns_handle = handle;
}

static void set_dhcps_dns(void)
{
    // Copy the upstream DNS server from STA to the AP DHCP server
    // so that AP clients can resolve DNS through the repeater
    esp_netif_dns_info_t dns;
    esp_err_t ret = esp_netif_get_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get STA DNS info");
        return;
    }

    char dns_str[16];
    snprintf(dns_str, sizeof(dns_str), IPSTR, IP2STR(&dns.ip.u_addr.ip4));
    ESP_LOGI(TAG, "Propagating upstream DNS to AP DHCP: %s", dns_str);

    // Set DNS on AP netif without restarting DHCP server (preserves leases)
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns));
}

static void apply_port_forwarding(void)
{
    /* NAPT port forwarding (works for traffic ROUTED through the ESP32).
     * Does NOT work for traffic from the same LAN destined to the STA IP,
     * because that arrives as local INPUT, not FORWARD. */
    /* Collect external IPs to bind forward rules to */
    uint32_t ext_ips[6];
    int ext_count = 0;
    ext_ips[ext_count++] = s_status.sta_ip;

    /* Add Tailscale VPN IP if connected */
    tailscale_status_t ts = {0};
    tailscale_manager_get_status(&ts);
    if (ts.connected && ts.vpn_ip[0] != '\0') {
        uint32_t vpn_ip = ipaddr_addr(ts.vpn_ip);
        if (vpn_ip != IPADDR_NONE && vpn_ip != 0) {
            ext_ips[ext_count++] = vpn_ip;
        }
    }

    for (int i = 0; i < CFG_PORT_FWD_MAX; i++) {
        if (!s_config->port_fwd[i].enabled || s_config->port_fwd[i].ext_port == 0) continue;

        u8_t proto = s_config->port_fwd[i].proto == 1 ? 17 : 6; // UDP=17, TCP=6
        u32_t dest_ip = s_config->port_fwd[i].int_ip;
        u16_t ext_port = s_config->port_fwd[i].ext_port;
        u16_t int_port = s_config->port_fwd[i].int_port;
        char ip_str[16];
        esp_ip4_addr_t ip = { .addr = dest_ip };
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip));

        for (int e = 0; e < ext_count; e++) {
            if (ext_ips[e] == 0) continue;
            if (ip_portmap_add(proto, ext_ips[e], ext_port, dest_ip, int_port)) {
                char ext_str[16];
                esp_ip4_addr_t eip = { .addr = ext_ips[e] };
                snprintf(ext_str, sizeof(ext_str), IPSTR, IP2STR(&eip));
                ESP_LOGI(TAG, "Port forward (NAPT): %s:%d -> %s:%d (%s)",
                         ext_str, ext_port, ip_str, int_port,
                         proto == 17 ? "UDP" : "TCP");
            } else {
                ESP_LOGW(TAG, "Port forward NAPT rule %d (IP %d) failed", i, e);
            }
        }
    }
}

static void disable_napt(void)
{
    if (s_napt_if == NAPT_IF_AP) {
        esp_netif_napt_disable(s_ap_netif);
        ESP_LOGI(TAG, "NAPT disabled on AP interface");
    }
    s_napt_if = NAPT_IF_NONE;
}

static esp_err_t enable_repeater_napt(void)
{
    if (s_napt_if == NAPT_IF_AP) return ESP_OK;

    disable_napt();
    esp_err_t ret = esp_netif_napt_enable(s_ap_netif);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable NAPT on AP interface: %s", esp_err_to_name(ret));
        return ret;
    }

    s_napt_if = NAPT_IF_AP;
    ESP_LOGI(TAG, "NAPT enabled on AP interface");
    apply_port_forwarding();
    start_proxy_listeners();
    return ESP_OK;
}

static esp_err_t enable_gateway_snat(void)
{
    stop_proxy_listeners();
    disable_napt();
    ESP_LOGW(TAG, "AP repeater NAPT/port forwarding disabled; Tailscale SNAT is managed on WireGuard.");
    return ESP_OK;
}

void wifi_manager_apply_port_forwarding(void)
{
    if (s_config && s_config->tailscale.net_mode == NET_MODE_TS_GATEWAY) {
        ESP_LOGW(TAG, "Port forwarding to AP clients is unavailable in Tailscale Gateway mode");
        stop_proxy_listeners();
        return;
    }
    stop_proxy_listeners();
    apply_port_forwarding();
    start_proxy_listeners();
}

esp_err_t wifi_manager_set_subnet_routing(bool enable)
{
    if (enable == s_subnet_routing) return ESP_OK;

    /* Exposing the LAN is handled as Gateway/SNAT by product policy. Gateway
     * disables AP NAPT/port forwarding here; MicroLink enables NAPT on the
     * WireGuard netif once Tailscale is connected. */
    s_subnet_routing = enable;
    if (enable) {
        if (s_config && s_config->tailscale.net_mode == NET_MODE_TS_GATEWAY) {
            ESP_LOGW(TAG, "Gateway mode: AP repeater NAPT/port forwarding disabled; Tailscale SNAT enabled.");
        } else {
            ESP_LOGW(TAG, "Repeater mode: Tailscale routes are advertised without SNAT; LAN return route required.");
            ESP_LOGW(TAG, "Add static route 100.64.0.0/10 via this device LAN IP: %d.%d.%d.%d",
                     (int)((s_status.sta_ip      ) & 0xff),
                     (int)((s_status.sta_ip >>  8) & 0xff),
                     (int)((s_status.sta_ip >> 16) & 0xff),
                     (int)((s_status.sta_ip >> 24) & 0xff));
        }
    } else {
        ESP_LOGI(TAG, "Subnet routing DISABLED");
    }
    return ESP_OK;
}

/* ============================================================================
 * TCP Proxy - escucha en puertos externos y reenvia a destinos internos.
 * Soluciona el problema del trafico INPUT (misma LAN) que no pasa por NAPT.
 * Soporta TCP (UDP no es practico sin buffer de sesiones).
 * ========================================================================== */

static void proxy_handle_client(int client_fd, const struct sockaddr_in *dest)
{
    int up_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (up_fd < 0) {
        close(client_fd);
        return;
    }

    struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(up_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(up_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(up_fd, (struct sockaddr *)dest, sizeof(*dest)) < 0) {
        close(up_fd);
        close(client_fd);
        return;
    }

    /* Bidirectional copy: client → upstream, upstream → client */
    int max_fd = (client_fd > up_fd ? client_fd : up_fd) + 1;
    uint8_t *buf = malloc(PROXY_BUF_SIZE);
    if (!buf) { close(up_fd); close(client_fd); return; }

    while (1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(client_fd, &rfds);
        FD_SET(up_fd, &rfds);

        struct timeval sel_tv = { .tv_sec = 30, .tv_usec = 0 };
        int ret = select(max_fd, &rfds, NULL, NULL, &sel_tv);
        if (ret <= 0) break;

        if (FD_ISSET(client_fd, &rfds)) {
            int n = read(client_fd, buf, PROXY_BUF_SIZE);
            if (n <= 0) break;
            if (write(up_fd, buf, n) < 0) break;
        }

        if (FD_ISSET(up_fd, &rfds)) {
            int n = read(up_fd, buf, PROXY_BUF_SIZE);
            if (n <= 0) break;
            if (write(client_fd, buf, n) < 0) break;
        }
    }

    free(buf);
    close(up_fd);
    close(client_fd);
}

static void proxy_listener_task(void *arg)
{
    int idx = (int)(intptr_t)arg;
    proxy_listener_t *pl = &s_proxy_listeners[idx];

    while (s_proxy_running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(pl->listen_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        ESP_LOGI(TAG, "Proxy: accepted from %s:%d -> %s:%d",
                 inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port),
                 inet_ntoa(pl->dest.sin_addr), ntohs(pl->dest.sin_port));

        proxy_handle_client(client_fd, &pl->dest);
    }
    vTaskDelete(NULL);
}

static void start_proxy_listeners(void)
{
    if (s_proxy_running) return;
    s_proxy_running = true;

    for (int i = 0; i < CFG_PORT_FWD_MAX; i++) {
        if (!s_config->port_fwd[i].enabled || s_config->port_fwd[i].ext_port == 0) continue;
        if (s_config->port_fwd[i].proto == 1) continue;  /* UDP not proxied */

        int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd < 0) {
            ESP_LOGW(TAG, "Proxy: cannot create socket for rule %d", i);
            continue;
        }

        int reuse = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        struct sockaddr_in addr = {
            .sin_family = AF_INET,
            .sin_port = htons(s_config->port_fwd[i].ext_port),
            .sin_addr.s_addr = htonl(INADDR_ANY),
        };

        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            ESP_LOGW(TAG, "Proxy: cannot bind port %d (in use?)",
                     s_config->port_fwd[i].ext_port);
            close(fd);
            continue;
        }

        listen(fd, 3);

        s_proxy_listeners[i].listen_fd = fd;
        s_proxy_listeners[i].dest.sin_family = AF_INET;
        s_proxy_listeners[i].dest.sin_port = htons(s_config->port_fwd[i].int_port);
        s_proxy_listeners[i].dest.sin_addr.s_addr = s_config->port_fwd[i].int_ip;

        char ip_str[16];
        esp_ip4_addr_t ip = { .addr = s_config->port_fwd[i].int_ip };
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip));
        ESP_LOGI(TAG, "Proxy: 0.0.0.0:%d -> %s:%d",
                 s_config->port_fwd[i].ext_port, ip_str,
                 s_config->port_fwd[i].int_port);

        xTaskCreate(proxy_listener_task, "tcp_proxy", 4096,
                    (void *)(intptr_t)i, 3, &s_proxy_listeners[i].task);
    }
}

static void stop_proxy_listeners(void)
{
    s_proxy_running = false;
    for (int i = 0; i < CFG_PORT_FWD_MAX; i++) {
        if (s_proxy_listeners[i].listen_fd > 0) {
            close(s_proxy_listeners[i].listen_fd);
            s_proxy_listeners[i].listen_fd = 0;
        }
        if (s_proxy_listeners[i].task) {
            vTaskDelete(s_proxy_listeners[i].task);
            s_proxy_listeners[i].task = NULL;
        }
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            if (strlen(s_config->sta_ssid) > 0 && !s_sta_paused) {
                ESP_LOGI(TAG, "STA started, scheduling connect to '%s'...", s_config->sta_ssid);
                schedule_reconnect(1);
            }
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
            ESP_LOGW(TAG, "STA disconnected (reason=%d)", event->reason);
            s_status.sta_connected = false;
            s_status.sta_rssi = 0;
            s_status.sta_ip = 0;
            tailscale_manager_on_sta_disconnected();

            // Disable any active NAPT/SNAT when STA disconnects.
            disable_napt();

            restart_captive_dns();

            if (s_reconfiguring) {
                ESP_LOGI(TAG, "STA disconnect during WiFi reconfiguration; reconnect deferred");
                break;
            }
            if (s_scanning) {
                ESP_LOGI(TAG, "STA disconnect during scan; reconnect deferred");
                break;
            }
            if (s_sta_paused) {
                ESP_LOGI(TAG, "STA reconnect paused");
                break;
            }

            if (s_retry_count < UINT8_MAX) {
                s_retry_count++;
            }

            if (s_config->sta_retry_max > 0 && s_retry_count >= s_config->sta_retry_max) {
                s_sta_recovery = true;
                ESP_LOGW(TAG, "STA recovery mode: retry_count=%u cooldown=%u s",
                         s_retry_count, WIFI_RECOVERY_COOLDOWN_S);
                schedule_reconnect(WIFI_RECOVERY_COOLDOWN_S);
            } else {
                s_sta_recovery = false;
                schedule_reconnect(reconnect_backoff_s());
            }
            break;
        }
        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *ev = (wifi_event_ap_staconnected_t *)event_data;
            char mac_str[18];
            snprintf(mac_str, sizeof(mac_str), MACSTR, MAC2STR(ev->mac));
            ESP_LOGI(TAG, "AP: station %s joined (AID=%d)", mac_str, ev->aid);
            s_status.ap_client_count++;
            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t *ev = (wifi_event_ap_stadisconnected_t *)event_data;
            char mac_str[18];
            snprintf(mac_str, sizeof(mac_str), MACSTR, MAC2STR(ev->mac));
            ESP_LOGI(TAG, "AP: station %s left (AID=%d)", mac_str, ev->aid);
            if (s_status.ap_client_count > 0) s_status.ap_client_count--;
            break;
        }
        case WIFI_EVENT_SCAN_DONE:
            if (s_scan_done) xSemaphoreGive(s_scan_done);
            break;
        default:
            break;
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            s_status.sta_connected = true;
            s_status.sta_ip = event->ip_info.ip.addr;
            s_retry_count = 0;
            s_sta_recovery = false;
            stop_reconnect_timer();
            char ip_str[16];
            snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&event->ip_info.ip));
            ESP_LOGI(TAG, "STA got IP: %s", ip_str);

            // Stop captive portal DNS — let real DNS work through upstream
            if (s_dns_handle) {
                stop_dns_server(s_dns_handle);
                s_dns_handle = NULL;
                ESP_LOGI(TAG, "Captive portal DNS stopped (STA connected)");
            }

            // Propagate upstream DNS to AP DHCP server
            set_dhcps_dns();

            // Set STA as default netif for routing
            esp_netif_set_default_netif(s_sta_netif);
            tailscale_manager_on_sta_got_ip();

            if (s_config->tailscale.net_mode == NET_MODE_TS_GATEWAY) {
                stop_proxy_listeners();
                enable_gateway_snat();
            } else {
                enable_repeater_napt();
            }
        }
    }
}

esp_err_t wifi_manager_init(repeater_config_t *config)
{
    s_config = config;
    s_scan_done = xSemaphoreCreateBinary();
    s_reconnect_timer = xTimerCreate("wifi_reconn", pdMS_TO_TICKS(1000),
                                     pdFALSE, NULL, reconnect_timer_cb);
    if (!s_scan_done || !s_reconnect_timer) {
        ESP_LOGE(TAG, "Failed to create WiFi manager synchronization primitives");
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    apply_netif_hostname();

    // Configure DHCP server to offer DNS before it starts (avoids restart later)
    uint8_t dhcps_offer_option = DHCPS_OFFER_DNS;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_stop(s_ap_netif));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET,
        ESP_NETIF_DOMAIN_NAME_SERVER, &dhcps_offer_option, sizeof(dhcps_offer_option)));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_start(s_ap_netif));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                         &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                         &wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    return ESP_OK;
}

static void configure_sta(void)
{
    wifi_config_t sta_cfg = {0};
    strlcpy((char *)sta_cfg.sta.ssid, s_config->sta_ssid, sizeof(sta_cfg.sta.ssid));
    sta_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta_cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    if (s_config->sta_eap_enabled) {
        sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_ENTERPRISE;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));

        ESP_ERROR_CHECK(esp_eap_client_set_identity((uint8_t *)s_config->sta_eap_identity,
                                                     strlen(s_config->sta_eap_identity)));
        ESP_ERROR_CHECK(esp_eap_client_set_username((uint8_t *)s_config->sta_eap_username,
                                                     strlen(s_config->sta_eap_username)));
        ESP_ERROR_CHECK(esp_eap_client_set_password((uint8_t *)s_config->sta_eap_password,
                                                     strlen(s_config->sta_eap_password)));
        ESP_ERROR_CHECK(esp_eap_client_set_disable_time_check(true));
        ESP_ERROR_CHECK(esp_wifi_sta_enterprise_enable());
        ESP_LOGI(TAG, "EAP configured: identity='%s' user='%s'",
                 s_config->sta_eap_identity, s_config->sta_eap_username);
    } else {
        strlcpy((char *)sta_cfg.sta.password, s_config->sta_pass, sizeof(sta_cfg.sta.password));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    }

    strlcpy(s_status.sta_ssid, s_config->sta_ssid, sizeof(s_status.sta_ssid));
}

static void configure_ap(void)
{
    wifi_config_t ap_cfg = {0};
    strlcpy((char *)ap_cfg.ap.ssid, s_config->ap_ssid, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = strlen(s_config->ap_ssid);
    strlcpy((char *)ap_cfg.ap.password, s_config->ap_pass, sizeof(ap_cfg.ap.password));
    ap_cfg.ap.channel = s_config->ap_channel;
    ap_cfg.ap.max_connection = s_config->ap_max_conn;
    ap_cfg.ap.authmode = strlen(s_config->ap_pass) > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ap_cfg.ap.ssid_hidden = s_config->ap_hide_ssid ? 1 : 0;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_LOGI(TAG, "SoftAP configured: SSID='%s' hidden=%d channel=%d max_conn=%d",
             s_config->ap_ssid, s_config->ap_hide_ssid ? 1 : 0,
             s_config->ap_channel, s_config->ap_max_conn);
}

esp_err_t wifi_manager_start(void)
{
    apply_netif_hostname();
    ESP_RETURN_ON_ERROR(apply_custom_macs(), TAG, "Failed to apply custom MAC settings");

    configure_sta();
    configure_ap();
    ESP_ERROR_CHECK(esp_wifi_set_mode(s_ap_enabled ? WIFI_MODE_APSTA : WIFI_MODE_STA));

    ESP_ERROR_CHECK(esp_wifi_start());
    s_wifi_started = true;
    if (!s_sta_paused && s_sta_scheduler_enabled && s_config->sta_ssid[0] != '\0') {
        schedule_reconnect(1);
    }

    ESP_LOGI(TAG, "WiFi started: AP='%s' STA='%s'", s_config->ap_ssid, s_config->sta_ssid);
    return ESP_OK;
}

esp_err_t wifi_manager_reconfigure(repeater_config_t *config)
{
    s_config = config;
    s_status.sta_connected = false;
    s_status.sta_rssi = 0;
    s_status.sta_ip = 0;

    // Disable NAPT before reconfiguring
    stop_proxy_listeners();
    disable_napt();
    stop_reconnect_timer();
    s_retry_count = 0;
    s_sta_recovery = false;

    if (s_wifi_started) {
        s_reconfiguring = true;
        esp_wifi_disconnect();
        esp_wifi_sta_enterprise_disable();
        ESP_ERROR_CHECK(esp_wifi_stop());
        s_wifi_started = false;
        apply_netif_hostname();
        esp_err_t ret = apply_custom_macs();
        if (ret != ESP_OK) {
            s_reconfiguring = false;
            return ret;
        }
        configure_sta();
        configure_ap();
        ESP_ERROR_CHECK(esp_wifi_set_mode(s_ap_enabled ? WIFI_MODE_APSTA : WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
        s_wifi_started = true;
        s_reconfiguring = false;
        if (!s_sta_paused && s_sta_scheduler_enabled && s_config->sta_ssid[0] != '\0') {
            schedule_reconnect(1);
        }
    }

    ESP_LOGI(TAG, "WiFi reconfigured: AP='%s' STA='%s'", s_config->ap_ssid, s_config->sta_ssid);
    return ESP_OK;
}

esp_err_t wifi_manager_scan(wifi_ap_record_t *ap_records, uint16_t *ap_count)
{
    bool resume_reconnect = false;

    if (!s_status.sta_connected && !s_sta_paused && s_sta_scheduler_enabled && s_config &&
        s_config->sta_ssid[0] != '\0') {
        resume_reconnect = true;
        s_scanning = true;
        stop_reconnect_timer();
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_disconnect());
    }

    wifi_scan_config_t scan_cfg = {
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 300,
    };

    esp_err_t ret = esp_wifi_scan_start(&scan_cfg, false);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Scan start failed: %s", esp_err_to_name(ret));
        s_scanning = false;
        if (resume_reconnect) schedule_reconnect(1);
        return ret;
    }

    if (xSemaphoreTake(s_scan_done, pdMS_TO_TICKS(10000)) != pdTRUE) {
        ESP_LOGE(TAG, "Scan timeout");
        esp_wifi_scan_stop();
        s_scanning = false;
        if (resume_reconnect) schedule_reconnect(1);
        return ESP_ERR_TIMEOUT;
    }

    *ap_count = WIFI_SCAN_MAX_AP;
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(ap_count, ap_records));
    ESP_LOGI(TAG, "Scan found %d APs", *ap_count);
    s_scanning = false;
    if (resume_reconnect) schedule_reconnect(1);
    return ESP_OK;
}

void wifi_manager_get_status(wifi_status_t *status)
{
    if (s_status.sta_connected) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            s_status.sta_rssi = ap_info.rssi;
        }
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_get_mac(WIFI_IF_STA, s_status.sta_mac));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_get_mac(WIFI_IF_AP, s_status.ap_mac));
    s_status.sta_retry_count = s_retry_count;
    s_status.sta_recovery = s_sta_recovery;
    s_status.sta_paused = s_sta_paused;
    s_status.sta_scheduler_enabled = s_sta_scheduler_enabled;
    s_status.ap_enabled = s_ap_enabled;
    s_status.sta_next_retry_s = 0;
    if (s_next_reconnect_tick != 0) {
        TickType_t now = xTaskGetTickCount();
        if (s_next_reconnect_tick > now) {
            TickType_t remaining = s_next_reconnect_tick - now;
            s_status.sta_next_retry_s = (uint16_t)((remaining + pdMS_TO_TICKS(999)) / pdMS_TO_TICKS(1000));
        }
    }
    memcpy(status, &s_status, sizeof(wifi_status_t));
}

esp_err_t wifi_manager_pause_sta(void)
{
    s_sta_paused = true;
    s_sta_recovery = false;
    stop_reconnect_timer();
    if (s_wifi_started) {
        esp_err_t ret = esp_wifi_disconnect();
        if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_CONNECT) {
            ESP_LOGW(TAG, "STA pause disconnect failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }
    ESP_LOGI(TAG, "STA reconnect paused");
    return ESP_OK;
}

esp_err_t wifi_manager_resume_sta(void)
{
    s_sta_paused = false;
    s_sta_recovery = false;
    s_retry_count = 0;
    stop_reconnect_timer();
    if (s_wifi_started && s_sta_scheduler_enabled && s_config &&
        s_config->sta_ssid[0] != '\0' && !s_status.sta_connected) {
        schedule_reconnect(1);
    }
    ESP_LOGI(TAG, "STA reconnect resumed");
    return ESP_OK;
}

esp_err_t wifi_manager_set_sta_scheduler_enabled(bool enabled)
{
    if (s_sta_scheduler_enabled == enabled) {
        return ESP_OK;
    }

    s_sta_scheduler_enabled = enabled;
    s_sta_recovery = false;
    stop_reconnect_timer();

    if (!enabled) {
        if (s_wifi_started) {
            esp_err_t ret = esp_wifi_disconnect();
            if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_CONNECT) {
                ESP_LOGW(TAG, "Scheduled STA disable disconnect failed: %s", esp_err_to_name(ret));
                return ret;
            }
        }
        ESP_LOGI(TAG, "STA disabled by scheduler");
        return ESP_OK;
    }

    s_retry_count = 0;
    if (s_wifi_started && !s_sta_paused && s_config &&
        s_config->sta_ssid[0] != '\0' && !s_status.sta_connected) {
        schedule_reconnect(1);
    }
    ESP_LOGI(TAG, "STA enabled by scheduler");
    return ESP_OK;
}

esp_err_t wifi_manager_set_ap_enabled(bool enabled)
{
    if (s_ap_enabled == enabled) {
        return ESP_OK;
    }

    s_ap_enabled = enabled;
    if (!enabled) {
        s_status.ap_client_count = 0;
    }
    if (!s_wifi_started) {
        ESP_LOGI(TAG, "SoftAP radio pending state: %s", enabled ? "enabled" : "disabled");
        return ESP_OK;
    }

    esp_err_t ret;
    if (enabled) {
        configure_ap();
        ret = esp_wifi_set_mode(WIFI_MODE_APSTA);
    } else {
        ret = esp_wifi_set_mode(WIFI_MODE_STA);
    }

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SoftAP radio change failed: %s", esp_err_to_name(ret));
        s_ap_enabled = !enabled;
        return ret;
    }

    ESP_LOGI(TAG, "SoftAP radio %s", enabled ? "enabled" : "disabled");
    return ESP_OK;
}

// --- Ping implementation ---

static SemaphoreHandle_t s_ping_done = NULL;
static SemaphoreHandle_t s_ping_mutex = NULL;
static ping_result_t s_ping_result;

static void ping_success_cb(esp_ping_handle_t hdl, void *args)
{
    uint32_t elapsed;
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed, sizeof(elapsed));
    ip_addr_t target_addr;
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target_addr, sizeof(target_addr));

    s_ping_result.success = true;
    s_ping_result.elapsed_ms = elapsed;
    s_ping_result.addr = target_addr.u_addr.ip4.addr;
}

static void ping_timeout_cb(esp_ping_handle_t hdl, void *args)
{
    s_ping_result.success = false;
}

static void ping_end_cb(esp_ping_handle_t hdl, void *args)
{
    if (s_ping_done) xSemaphoreGive(s_ping_done);
}

esp_err_t wifi_manager_ping(const char *target, ping_result_t *result)
{
    if (!s_ping_done) {
        s_ping_done = xSemaphoreCreateBinary();
    }
    if (!s_ping_mutex) {
        s_ping_mutex = xSemaphoreCreateMutex();
    }

    // Serialize ping requests to avoid exhausting sockets
    if (xSemaphoreTake(s_ping_mutex, pdMS_TO_TICKS(15000)) != pdTRUE) {
        result->success = false;
        return ESP_ERR_TIMEOUT;
    }

    memset(&s_ping_result, 0, sizeof(s_ping_result));

    // Resolve hostname
    struct addrinfo hint = { .ai_family = AF_INET };
    struct addrinfo *res = NULL;
    if (getaddrinfo(target, NULL, &hint, &res) != 0 || res == NULL) {
        ESP_LOGE(TAG, "DNS lookup failed for '%s'", target);
        result->success = false;
        xSemaphoreGive(s_ping_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    struct in_addr addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    freeaddrinfo(res);

    ip_addr_t target_addr;
    target_addr.type = IPADDR_TYPE_V4;
    target_addr.u_addr.ip4.addr = addr.s_addr;

    esp_ping_config_t ping_config = ESP_PING_DEFAULT_CONFIG();
    ping_config.target_addr = target_addr;
    ping_config.count = 3;
    ping_config.interval_ms = 500;
    ping_config.timeout_ms = 2000;
    ping_config.task_stack_size = 4096;

    esp_ping_callbacks_t cbs = {
        .on_ping_success = ping_success_cb,
        .on_ping_timeout = ping_timeout_cb,
        .on_ping_end = ping_end_cb,
    };

    esp_ping_handle_t hdl;
    esp_err_t ret = esp_ping_new_session(&ping_config, &cbs, &hdl);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Ping session creation failed");
        result->success = false;
        xSemaphoreGive(s_ping_mutex);
        return ret;
    }

    esp_ping_start(hdl);

    // Wait for ping to finish (3 pings * 2.5s max each = 7.5s worst case)
    if (xSemaphoreTake(s_ping_done, pdMS_TO_TICKS(10000)) != pdTRUE) {
        ESP_LOGE(TAG, "Ping timeout");
        esp_ping_stop(hdl);
        esp_ping_delete_session(hdl);
        result->success = false;
        xSemaphoreGive(s_ping_mutex);
        return ESP_ERR_TIMEOUT;
    }

    esp_ping_stop(hdl);
    esp_ping_delete_session(hdl);

    memcpy(result, &s_ping_result, sizeof(ping_result_t));
    xSemaphoreGive(s_ping_mutex);
    return ESP_OK;
}
