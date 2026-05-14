#include "tailscale_manager.h"
#include "wifi_manager.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <string.h>

#if CONFIG_IDF_TARGET_ESP32S3
#include "microlink.h"
#endif

static const char *TAG = "tailscale_manager";

#if CONFIG_IDF_TARGET_ESP32S3
#define TAILSCALE_MANAGER_AVAILABLE true
#else
#define TAILSCALE_MANAGER_AVAILABLE false
#endif

static tailscale_config_t s_config;
static tailscale_state_t s_state = TAILSCALE_STATE_DISABLED;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_worker_task;
static bool s_initialized;
static bool s_sta_has_ip;
static bool s_started;
static bool s_deferred_stop;
static bool s_scheduler_enabled = true;
static esp_timer_handle_t s_boot_timer;
static esp_err_t s_last_error = ESP_OK;

#if CONFIG_IDF_TARGET_ESP32S3
static microlink_t *s_ml;
static char s_ml_auth_key[CFG_TS_AUTH_KEY_LEN];
static char s_ml_device_name[CFG_TS_NAME_LEN];
static char s_ml_control_host[64];
#endif

#if CONFIG_IDF_TARGET_ESP32S3
static const char *tailscale_subnet_route_state_to_string(microlink_subnet_route_state_t state)
{
    switch (state) {
    case ML_SUBNET_ROUTE_NOT_ADVERTISED:
        return "not_advertised";
    case ML_SUBNET_ROUTE_PENDING:
        return "pending";
    case ML_SUBNET_ROUTE_APPROVED:
        return "approved";
    case ML_SUBNET_ROUTE_ACTIVE:
        return "active";
    case ML_SUBNET_ROUTE_UNKNOWN:
    default:
        return "unknown";
    }
}
#endif

typedef enum {
    TAILSCALE_WORKER_START_OR_REBIND = 1,
    TAILSCALE_WORKER_STOP = 2,
} tailscale_worker_op_t;

static void tailscale_lock(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void tailscale_unlock(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

static bool tailscale_is_configured(const tailscale_config_t *config)
{
    return config != NULL && config->auth_key[0] != '\0';
}

static tailscale_state_t tailscale_state_from_microlink(int state)
{
#if CONFIG_IDF_TARGET_ESP32S3
    switch ((microlink_state_t)state) {
    case ML_STATE_WIFI_WAIT:
        return TAILSCALE_STATE_WAITING_FOR_WIFI;
    case ML_STATE_CONNECTING:
    case ML_STATE_REGISTERING:
        return TAILSCALE_STATE_CONNECTING;
    case ML_STATE_CONNECTED:
        return TAILSCALE_STATE_CONNECTED;
    case ML_STATE_RECONNECTING:
        return TAILSCALE_STATE_RECONNECTING;
    case ML_STATE_ERROR:
        return TAILSCALE_STATE_ERROR;
    case ML_STATE_IDLE:
    default:
        return TAILSCALE_STATE_STARTING;
    }
#else
    (void)state;
    return TAILSCALE_STATE_DISABLED;
#endif
}

static void tailscale_refresh_state_locked(void)
{
    if (!TAILSCALE_MANAGER_AVAILABLE) {
        s_state = TAILSCALE_STATE_DISABLED;
        return;
    }

    if (!s_config.enabled) {
        s_state = TAILSCALE_STATE_DISABLED;
        return;
    }

    if (!s_scheduler_enabled) {
        s_state = TAILSCALE_STATE_DISABLED;
        return;
    }

    if (!tailscale_is_configured(&s_config)) {
        s_state = TAILSCALE_STATE_NOT_CONFIGURED;
        return;
    }

    if (!s_sta_has_ip) {
        s_state = s_started ? TAILSCALE_STATE_RECONNECTING : TAILSCALE_STATE_WAITING_FOR_WIFI;
        return;
    }

    if (!s_started) {
        s_state = TAILSCALE_STATE_STARTING;
    }
}

#if CONFIG_IDF_TARGET_ESP32S3
static void tailscale_log_heap(const char *point)
{
    ESP_LOGI(TAG, "Heap %s: internal=%lu psram=%lu",
             point,
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static void tailscale_apply_gateway_snat(microlink_t *ml, bool expose_lan, network_mode_t net_mode)
{
    if (!ml) return;

    bool enable = expose_lan && net_mode == NET_MODE_TS_GATEWAY;
    esp_err_t ret = microlink_set_wg_napt(ml, enable);
    if (ret == ESP_OK) {
        if (enable) {
            ESP_LOGW(TAG, "AP repeater NAPT/port forwarding disabled; Tailscale SNAT enabled on WireGuard.");
            microlink_log_gateway_diagnostics(ml, "tailscale_gateway_connected");
        } else {
            ESP_LOGI(TAG, "Tailscale WireGuard SNAT disabled");
        }
    }
}

static void tailscale_state_callback(microlink_t *ml, microlink_state_t state, void *user_data)
{
    (void)user_data;

    tailscale_lock();
    if (ml == s_ml) {
        s_state = tailscale_state_from_microlink(state);
        s_started = state != ML_STATE_IDLE;
        bool expose_lan = s_config.expose_lan;
        network_mode_t net_mode = s_config.net_mode;
        ESP_LOGI(TAG, "MicroLink state: %s", tailscale_manager_state_to_string(s_state));

        /* Re-apply port forwarding with VPN IP when we connect */
        if (state == ML_STATE_CONNECTED) {
            tailscale_unlock();
            wifi_manager_apply_port_forwarding();
            if (expose_lan) {
                wifi_manager_set_subnet_routing(true);
            }
            tailscale_apply_gateway_snat(ml, expose_lan, net_mode);
            return;
        }
    }
    tailscale_unlock();
}

static void tailscale_worker(void *arg)
{
    tailscale_worker_op_t op = (tailscale_worker_op_t)(intptr_t)arg;

    if (op == TAILSCALE_WORKER_STOP) {
        microlink_t *ml = NULL;

        tailscale_lock();
        ml = s_ml;
        s_ml = NULL;
        s_started = false;
        s_deferred_stop = false;
        tailscale_refresh_state_locked();
        tailscale_unlock();

        if (ml) {
            tailscale_log_heap("before MicroLink stop");
            microlink_set_wg_napt(ml, false);
            microlink_destroy(ml);
            tailscale_log_heap("after MicroLink stop");
        }

        tailscale_lock();
        s_worker_task = NULL;
        tailscale_unlock();
        vTaskDelete(NULL);
        return;
    }

    tailscale_config_t cfg;
    bool do_rebind = false;
    microlink_t *ml = NULL;

    tailscale_lock();
    if (!TAILSCALE_MANAGER_AVAILABLE || !s_config.enabled || !s_scheduler_enabled ||
        !tailscale_is_configured(&s_config) || !s_sta_has_ip) {
        tailscale_refresh_state_locked();
        s_worker_task = NULL;
        tailscale_unlock();
        vTaskDelete(NULL);
        return;
    }

    cfg = s_config;
    if (s_ml && s_started) {
        ml = s_ml;
        do_rebind = true;
        s_state = TAILSCALE_STATE_RECONNECTING;
    } else {
        strlcpy(s_ml_auth_key, cfg.auth_key, sizeof(s_ml_auth_key));
        strlcpy(s_ml_device_name, cfg.device_name, sizeof(s_ml_device_name));
        strlcpy(s_ml_control_host, cfg.control_host, sizeof(s_ml_control_host));
        s_state = TAILSCALE_STATE_STARTING;
    }
    tailscale_unlock();

    if (do_rebind) {
        ESP_LOGI(TAG, "Rebinding MicroLink after WiFi change");
        tailscale_log_heap("before MicroLink rebind");
        esp_err_t ret = microlink_rebind(ml);
        tailscale_log_heap("after MicroLink rebind");

        microlink_t *stop_ml = NULL;
        tailscale_lock();
        if (s_deferred_stop || !s_config.enabled || !s_scheduler_enabled ||
            !tailscale_is_configured(&s_config)) {
            stop_ml = s_ml;
            s_ml = NULL;
            s_started = false;
            s_deferred_stop = false;
            tailscale_refresh_state_locked();
        } else {
            s_last_error = ret;
            s_state = (ret == ESP_OK) ? TAILSCALE_STATE_RECONNECTING : TAILSCALE_STATE_ERROR;
        }
        s_worker_task = NULL;
        tailscale_unlock();

        if (stop_ml) {
            tailscale_log_heap("before deferred MicroLink stop");
            microlink_set_wg_napt(stop_ml, false);
            microlink_destroy(stop_ml);
            tailscale_log_heap("after deferred MicroLink stop");
        }

        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Starting MicroLink runtime");
    tailscale_log_heap("before MicroLink init");

    /* Build advertise routes JSON string if subnet routing is enabled */
    char adv_routes[128] = "";
    if (cfg.expose_lan && cfg.advertise_cidr[0] != '\0') {
        snprintf(adv_routes, sizeof(adv_routes), "[\"%s\"]", cfg.advertise_cidr);
        ESP_LOGI(TAG, "Advertise routes: %s", adv_routes);
        if (cfg.net_mode == NET_MODE_TS_GATEWAY) {
            ESP_LOGW(TAG, "Gateway mode: AP repeater NAPT/port forwarding disabled; Tailscale SNAT enabled.");
        } else {
            ESP_LOGW(TAG, "Repeater mode: Tailscale routes are advertised without SNAT; LAN return route required.");
        }
    }

    microlink_config_t ml_config = {
        .auth_key = s_ml_auth_key,
        .device_name = s_ml_device_name,
        .control_host = s_ml_control_host,
        .enable_derp = cfg.enable_derp,
        .enable_stun = cfg.enable_stun,
        .enable_disco = cfg.enable_disco,
        .max_peers = cfg.max_peers,
        .wifi_tx_power_dbm = 0,
        .priority_peer_ip = 0,
        .disco_heartbeat_ms = 0,
        .stun_interval_ms = 0,
        .ctrl_watchdog_ms = 0,
        .advertise_routes = adv_routes[0] ? adv_routes : NULL,
        .enable_subnet_routing = false,
    };

    ml = microlink_init(&ml_config);
    if (!ml) {
        tailscale_lock();
        s_last_error = ESP_FAIL;
        s_state = TAILSCALE_STATE_ERROR;
        s_worker_task = NULL;
        tailscale_unlock();
        tailscale_log_heap("after failed MicroLink init");
        vTaskDelete(NULL);
        return;
    }

    microlink_set_state_callback(ml, tailscale_state_callback, NULL);

    esp_err_t ret = microlink_start(ml);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MicroLink start failed: %s", esp_err_to_name(ret));
        microlink_destroy(ml);
        tailscale_lock();
        s_last_error = ret;
        s_state = TAILSCALE_STATE_ERROR;
        s_worker_task = NULL;
        tailscale_unlock();
        tailscale_log_heap("after failed MicroLink start");
        vTaskDelete(NULL);
        return;
    }

    tailscale_log_heap("after MicroLink start");

    microlink_t *running_ml = NULL;
    bool connected_after_start = false;
    tailscale_lock();
    bool keep_running = !s_deferred_stop && s_config.enabled && s_scheduler_enabled &&
                        tailscale_is_configured(&s_config) && s_sta_has_ip;
    s_deferred_stop = false;
    if (keep_running) {
        s_ml = ml;
        running_ml = ml;
        s_started = true;
        s_last_error = ESP_OK;
        microlink_state_t ml_state = microlink_get_state(ml);
        connected_after_start = ml_state == ML_STATE_CONNECTED;
        s_state = tailscale_state_from_microlink(ml_state);
        ml = NULL;
    } else {
        s_started = false;
        tailscale_refresh_state_locked();
    }
    s_worker_task = NULL;
    tailscale_unlock();

    if (connected_after_start) {
        wifi_manager_apply_port_forwarding();
        if (cfg.expose_lan) {
            wifi_manager_set_subnet_routing(true);
        }
        tailscale_apply_gateway_snat(running_ml, cfg.expose_lan, cfg.net_mode);
    }

    if (ml) {
        microlink_set_wg_napt(ml, false);
        microlink_destroy(ml);
        tailscale_log_heap("after deferred MicroLink stop");
    }

    vTaskDelete(NULL);
}
#endif

static void tailscale_schedule_worker(tailscale_worker_op_t op)
{
#if CONFIG_IDF_TARGET_ESP32S3
    tailscale_lock();
    if (s_worker_task) {
        if (op == TAILSCALE_WORKER_STOP) {
            s_deferred_stop = true;
        }
        tailscale_unlock();
        return;
    }

    BaseType_t ret = xTaskCreate(tailscale_worker, "tailscale_mgr", 12288,
                                 (void *)(intptr_t)op, 5, &s_worker_task);
    if (ret != pdPASS) {
        s_worker_task = NULL;
        s_last_error = ESP_ERR_NO_MEM;
        s_state = TAILSCALE_STATE_ERROR;
        ESP_LOGE(TAG, "Failed to create worker task");
    }
    tailscale_unlock();
#else
    (void)op;
#endif
}

#if CONFIG_IDF_TARGET_ESP32S3
static void boot_timer_cb(void *arg);
#endif

esp_err_t tailscale_manager_init(const repeater_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            return ESP_ERR_NO_MEM;
        }
    }

    /* Create boot delay timer (one-shot) */
#if CONFIG_IDF_TARGET_ESP32S3
    if (!s_boot_timer) {
        esp_timer_create_args_t targs = {
            .callback = boot_timer_cb,
            .name = "ts_boot_delay"
        };
        esp_timer_create(&targs, &s_boot_timer);
    }
#endif

    tailscale_lock();
    s_config = config->tailscale;
    s_sta_has_ip = false;
    s_last_error = ESP_OK;
    s_initialized = true;
    s_scheduler_enabled = true;
    tailscale_refresh_state_locked();
    tailscale_unlock();

    ESP_LOGI(TAG, "Initialized: available=%d enabled=%d configured=%d state=%s",
             TAILSCALE_MANAGER_AVAILABLE,
             s_config.enabled,
             tailscale_is_configured(&s_config),
             tailscale_manager_state_to_string(s_state));
    return ESP_OK;
}

esp_err_t tailscale_manager_apply_config(const repeater_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    tailscale_lock();
    s_config = config->tailscale;
    s_last_error = ESP_OK;
    s_initialized = true;
    bool should_stop = !s_config.enabled || !s_scheduler_enabled || !tailscale_is_configured(&s_config);
    bool should_start = s_config.enabled && s_scheduler_enabled &&
                        tailscale_is_configured(&s_config) && s_sta_has_ip;

    /* If MicroLink is already running, restart the device so the new config
     * (expose_lan, advertise_cidr) takes effect on next boot. */
    if (s_started && should_start) {
        tailscale_unlock();
        ESP_LOGI(TAG, "Config changed, restarting device...");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
        return ESP_OK;
    }

    if (should_stop) {
        tailscale_schedule_worker(TAILSCALE_WORKER_STOP);
    } else if (should_start) {
        tailscale_schedule_worker(TAILSCALE_WORKER_START_OR_REBIND);
    }

    ESP_LOGI(TAG, "Config applied: enabled=%d configured=%d state=%s",
             s_config.enabled,
             tailscale_is_configured(&s_config),
             tailscale_manager_state_to_string(s_state));
    return ESP_OK;
}

esp_err_t tailscale_manager_set_scheduler_enabled(bool enabled)
{
    bool should_stop = false;
    bool should_start = false;

    tailscale_lock();
    if (s_scheduler_enabled == enabled) {
        tailscale_unlock();
        return ESP_OK;
    }

    s_scheduler_enabled = enabled;
    s_last_error = ESP_OK;
    should_stop = !enabled && s_started;
    should_start = enabled && s_config.enabled && tailscale_is_configured(&s_config) && s_sta_has_ip;
    tailscale_refresh_state_locked();
    tailscale_unlock();

    if (should_stop) {
        ESP_LOGI(TAG, "Tailscale disabled by scheduler");
        tailscale_schedule_worker(TAILSCALE_WORKER_STOP);
    } else if (should_start) {
        ESP_LOGI(TAG, "Tailscale enabled by scheduler");
        tailscale_schedule_worker(TAILSCALE_WORKER_START_OR_REBIND);
    } else {
        ESP_LOGI(TAG, "Tailscale scheduler gate: %s", enabled ? "enabled" : "disabled");
    }

    return ESP_OK;
}

#if CONFIG_IDF_TARGET_ESP32S3
static void boot_timer_cb(void *arg)
{
    (void)arg;
    tailscale_lock();
    bool should_start = s_config.enabled && s_scheduler_enabled &&
                        tailscale_is_configured(&s_config) && s_sta_has_ip;
    tailscale_unlock();
    if (should_start) {
        ESP_LOGI(TAG, "Boot delay expired, starting Tailscale");
        tailscale_schedule_worker(TAILSCALE_WORKER_START_OR_REBIND);
    }
}

void tailscale_manager_on_sta_got_ip(void)
{
    tailscale_lock();
    s_sta_has_ip = true;
    bool should_start = s_config.enabled && s_scheduler_enabled &&
                        tailscale_is_configured(&s_config);

    /* On first STA_IP from boot, delay MicroLink startup until 60s.
     * Without this delay, concurrent MicroLink init + WiFi/lwIP during
     * early boot causes a crash ~6-9s after boot. */
    uint64_t boot_ms = esp_timer_get_time() / 1000;
    if (should_start && boot_ms < 60000) {
        ESP_LOGI(TAG, "Delaying Tailscale start (%llus from boot, waiting until 60s)",
                 (unsigned long long)(boot_ms / 1000));
        if (s_boot_timer) {
            esp_timer_start_once(s_boot_timer, (60 * 1000 * 1000) - (boot_ms * 1000));
        }
        should_start = false;
    }

    tailscale_refresh_state_locked();
    tailscale_unlock();

    if (should_start) {
        tailscale_schedule_worker(TAILSCALE_WORKER_START_OR_REBIND);
    }
}
#else
void tailscale_manager_on_sta_got_ip(void)
{
    /* No-op on non-S3 targets */
}
#endif

void tailscale_manager_on_sta_disconnected(void)
{
    tailscale_lock();
    s_sta_has_ip = false;
    if (s_state == TAILSCALE_STATE_CONNECTED ||
        s_state == TAILSCALE_STATE_CONNECTING ||
        s_state == TAILSCALE_STATE_STARTING) {
        s_state = TAILSCALE_STATE_RECONNECTING;
        tailscale_unlock();
        return;
    }
    tailscale_refresh_state_locked();
    tailscale_unlock();
}

void tailscale_manager_get_status(tailscale_status_t *status)
{
    if (status == NULL) {
        return;
    }

    tailscale_lock();
    memset(status, 0, sizeof(*status));
    status->available = TAILSCALE_MANAGER_AVAILABLE;
    status->enabled = s_config.enabled;
    status->scheduler_enabled = s_scheduler_enabled;
    status->configured = tailscale_is_configured(&s_config);
    status->started = s_started;
    status->state = s_initialized ? s_state : TAILSCALE_STATE_DISABLED;
    status->last_error = s_last_error;
    status->subnet_route_advertised = s_config.expose_lan && s_config.advertise_cidr[0] != '\0';
    strlcpy(status->advertised_cidr, s_config.advertise_cidr, sizeof(status->advertised_cidr));
    strlcpy(status->subnet_route_state,
            status->subnet_route_advertised ? "unknown" : "not_advertised",
            sizeof(status->subnet_route_state));
#if CONFIG_IDF_TARGET_ESP32S3
    if (s_ml) {
        microlink_state_t ml_state = microlink_get_state(s_ml);
        microlink_subnet_route_state_t route_state = microlink_get_subnet_route_state(s_ml);
        status->state = tailscale_state_from_microlink(ml_state);
        status->connected = microlink_is_connected(s_ml);
        status->started = ml_state != ML_STATE_IDLE;
        status->subnet_route_active = route_state == ML_SUBNET_ROUTE_ACTIVE;
        strlcpy(status->subnet_route_state,
                tailscale_subnet_route_state_to_string(route_state),
                sizeof(status->subnet_route_state));
        int peer_count = microlink_get_peer_count(s_ml);
        status->peer_count = peer_count > 255 ? 255 : (uint8_t)peer_count;
        uint32_t vpn_ip = microlink_get_vpn_ip(s_ml);
        if (vpn_ip != 0) {
            microlink_ip_to_str(vpn_ip, status->vpn_ip);
        }
    }
#else
    status->connected = false;
    status->peer_count = 0;
#endif
    status->heap_internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
#if CONFIG_SPIRAM
    status->heap_psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
#else
    status->heap_psram_free = 0;
#endif
    tailscale_unlock();
}

const char *tailscale_manager_state_to_string(tailscale_state_t state)
{
    switch (state) {
    case TAILSCALE_STATE_DISABLED:
        return "disabled";
    case TAILSCALE_STATE_NOT_CONFIGURED:
        return "not_configured";
    case TAILSCALE_STATE_WAITING_FOR_WIFI:
        return "waiting_for_wifi";
    case TAILSCALE_STATE_STARTING:
        return "starting";
    case TAILSCALE_STATE_CONNECTING:
        return "connecting";
    case TAILSCALE_STATE_CONNECTED:
        return "connected";
    case TAILSCALE_STATE_RECONNECTING:
        return "reconnecting";
    case TAILSCALE_STATE_ERROR:
        return "error";
    default:
        return "error";
    }
}
