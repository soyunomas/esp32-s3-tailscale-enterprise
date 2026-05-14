#include "factory_reset.h"

#include "config_storage.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "factory_reset";

#define FACTORY_RESET_GPIO GPIO_NUM_0
#define SAMPLE_MS          50U
#define VALID_MIN_MS       5000U
#define VALID_MAX_MS       10000U

static TaskHandle_t s_factory_reset_task = NULL;

static void factory_reset_task(void *arg)
{
    (void)arg;

    uint32_t held_ms = 0;
    uint32_t last_log_s = 0;
    bool cancelled = false;

    while (1) {
        bool pressed = gpio_get_level(FACTORY_RESET_GPIO) == 0;

        if (pressed) {
            held_ms += SAMPLE_MS;
            uint32_t held_s = held_ms / 1000U;

            if (held_s > last_log_s && held_s <= (VALID_MAX_MS / 1000U)) {
                last_log_s = held_s;
                ESP_LOGW(TAG, "BOOT held %lu s", (unsigned long)held_s);
            }

            if (!cancelled && held_ms > VALID_MAX_MS) {
                cancelled = true;
                ESP_LOGW(TAG, "BOOT held >10 s, factory reset cancelled until release");
            }
        } else {
            if (held_ms > 0) {
                if (!cancelled && held_ms >= VALID_MIN_MS && held_ms <= VALID_MAX_MS) {
                    ESP_LOGW(TAG, "BOOT released after %lu ms: OK factory reset",
                             (unsigned long)held_ms);
                    esp_err_t ret = config_storage_erase();
                    if (ret != ESP_OK) {
                        ESP_LOGE(TAG, "Factory reset erase failed: %s", esp_err_to_name(ret));
                    } else {
                        ESP_LOGW(TAG, "Factory reset complete, restarting");
                    }
                    vTaskDelay(pdMS_TO_TICKS(200));
                    esp_restart();
                } else if (cancelled) {
                    ESP_LOGW(TAG, "BOOT released after cancelled hold, ignoring");
                } else {
                    ESP_LOGI(TAG, "BOOT released after %lu ms, ignoring",
                             (unsigned long)held_ms);
                }
            }

            held_ms = 0;
            last_log_s = 0;
            cancelled = false;
        }

        vTaskDelay(pdMS_TO_TICKS(SAMPLE_MS));
    }
}

esp_err_t factory_reset_init(void)
{
    if (s_factory_reset_task) return ESP_OK;

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << FACTORY_RESET_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t ret = gpio_config(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure BOOT GPIO: %s", esp_err_to_name(ret));
        return ret;
    }

    BaseType_t ok = xTaskCreate(factory_reset_task, "factory_reset", 2048,
                                NULL, 3, &s_factory_reset_task);
    if (ok != pdPASS) {
        s_factory_reset_task = NULL;
        ESP_LOGE(TAG, "Failed to create factory reset task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "BOOT factory reset armed on GPIO0 (hold 5-10 s, release to reset)");
    return ESP_OK;
}
