#include "esp_log.h"
#include "config_storage.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "dns_server.h"
#include "log_buffer.h"
#include "tailscale_manager.h"
#include "factory_reset.h"
#include "scheduler_manager.h"

static const char *TAG = "main";
static repeater_config_t s_config;

void app_main(void)
{
    ESP_LOGI(TAG, "=== ESP32-S3 WiFi Repeater + Tailscale ===");

    // Initialize log buffer (must be before any heavy logging)
    ESP_ERROR_CHECK(log_buffer_init());

    // Initialize NVS
    ESP_ERROR_CHECK(config_storage_init());

    // Arm physical factory reset via BOOT button (GPIO0).
    ESP_ERROR_CHECK(factory_reset_init());

    // Load configuration
    config_storage_load(&s_config);

    // Apply runtime log levels right after loading config so the rest of the
    // boot already follows user preferences (lighter logs => faster connect).
    config_storage_apply_log_levels(&s_config);

    // Initialize Tailscale wrapper. This does not start MicroLink yet.
    ESP_ERROR_CHECK(tailscale_manager_init(&s_config));

    // Initialize and start WiFi
    ESP_ERROR_CHECK(wifi_manager_init(&s_config));
    ESP_ERROR_CHECK(wifi_manager_start());

    // Start time sync and scheduler after WiFi is up. Scheduler keeps AP on
    // until time is valid, then applies temporary automation state.
    ESP_ERROR_CHECK(scheduler_manager_init(&s_config));

    // Start web server
    ESP_ERROR_CHECK(web_server_start(&s_config));

    // Start DNS server for captive portal (will be stopped when STA connects)
    dns_server_config_t dns_config = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
    dns_server_handle_t dns_handle = start_dns_server(&dns_config);
    wifi_manager_set_dns_handle(dns_handle);

    ESP_LOGI(TAG, "System ready. AP='%s' Web UI at http://192.168.4.1", s_config.ap_ssid);
}
