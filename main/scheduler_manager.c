#include "scheduler_manager.h"
#include "wifi_manager.h"
#include "tailscale_manager.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "scheduler";

static repeater_config_t *s_config = NULL;
static SemaphoreHandle_t s_lock = NULL;
static bool s_ntp_started = false;
static bool s_ntp_synced = false;
static bool s_last_ap_effective = true;
static bool s_last_ap_desired = true;
static bool s_last_sta_effective = true;
static bool s_last_sta_desired = true;
static bool s_last_tailscale_effective = true;
static bool s_last_tailscale_desired = true;
static bool s_last_safety_hold = false;
static int s_last_active_rule = -1;
static char s_last_reason[SCHED_REASON_LEN] = "Always on";

static void scheduler_lock(void)
{
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void scheduler_unlock(void)
{
    if (s_lock) xSemaphoreGive(s_lock);
}

static bool time_is_valid(void)
{
    time_t now = time(NULL);
    return now >= 1700000000;
}

static void format_time(time_t when, char *out, size_t out_size)
{
    if (!time_is_valid()) {
        strlcpy(out, "--", out_size);
        return;
    }
    struct tm tm_now;
    localtime_r(&when, &tm_now);
    strftime(out, out_size, "%Y-%m-%d %H:%M", &tm_now);
}

static int tm_day_bit(const struct tm *tm_now)
{
    return (tm_now->tm_wday + 6) % 7; /* bit0=Mon */
}

static bool rule_matches_time(const scheduler_rule_t *rule, const struct tm *tm_now)
{
    if (!rule->enabled) return false;
    if ((rule->days & (1U << tm_day_bit(tm_now))) == 0) return false;

    uint16_t minute = (uint16_t)(tm_now->tm_hour * 60 + tm_now->tm_min);
    return minute >= rule->start_min && minute < rule->end_min;
}

static bool evaluate_schedule_at(time_t when, bool *ap_desired, bool *sta_desired,
                                 bool *tailscale_desired, int *active_rule)
{
    if (!s_config) return false;
    *ap_desired = true;
    *sta_desired = true;
    *tailscale_desired = true;
    *active_rule = -1;

    if (s_config->sched_mode == SCHED_MODE_ALWAYS_ON) {
        return true;
    }
    if (s_config->sched_mode == SCHED_MODE_MANUAL_OFF) {
        *ap_desired = false;
        return true;
    }
    if (s_config->sched_mode != SCHED_MODE_SCHEDULED) {
        return true;
    }

    struct tm tm_now;
    localtime_r(&when, &tm_now);
    for (int i = 0; i < CFG_SCHED_RULES_MAX; i++) {
        if (rule_matches_time(&s_config->sched_rules[i], &tm_now)) {
            *ap_desired = s_config->sched_rules[i].ap_enabled;
            *sta_desired = s_config->sched_rules[i].sta_enabled;
            *tailscale_desired = s_config->sched_rules[i].tailscale_enabled;
            *active_rule = i;
            return true;
        }
    }
    return true;
}

static void find_next_change(time_t now,
                             uint32_t *next_s, int *next_rule, time_t *next_time)
{
    *next_s = 0;
    *next_rule = -1;
    *next_time = 0;
    if (!time_is_valid() || !s_config) return;

    time_t scan = now - (now % 60) + 60;
    for (int i = 1; i <= 7 * 24 * 60; i++, scan += 60) {
        bool ap = true;
        bool sta = true;
        bool ts = true;
        int rule = -1;
        evaluate_schedule_at(scan, &ap, &sta, &ts, &rule);
        if (ap != s_last_ap_desired || sta != s_last_sta_desired ||
            ts != s_last_tailscale_desired || rule != s_last_active_rule) {
            *next_s = (uint32_t)(scan - now);
            *next_rule = rule;
            *next_time = scan;
            return;
        }
    }
}

static void apply_timezone_locked(void)
{
    if (!s_config) return;
    setenv("TZ", s_config->sched_tz, 1);
    tzset();
}

static void time_sync_cb(struct timeval *tv)
{
    (void)tv;
    s_ntp_synced = true;
    ESP_LOGI(TAG, "NTP time synchronized");
}

static void start_sntp_locked(void)
{
    if (s_ntp_started) return;
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    esp_sntp_set_time_sync_notification_cb(time_sync_cb);
    esp_sntp_init();
    s_ntp_started = true;
    ESP_LOGI(TAG, "SNTP started");
}

static void scheduler_evaluate(void)
{
    scheduler_lock();
    bool valid = time_is_valid();
    bool ap_desired = true;
    bool sta_desired = true;
    bool tailscale_desired = true;
    int active_rule = -1;
    const char *reason = "Always on";

    if (!valid) {
        ap_desired = true;
        sta_desired = true;
        tailscale_desired = true;
        reason = "Waiting for time sync";
    } else {
        time_t now = time(NULL);
        evaluate_schedule_at(now, &ap_desired, &sta_desired, &tailscale_desired, &active_rule);
        if (s_config && s_config->sched_mode == SCHED_MODE_MANUAL_OFF) {
            reason = "Manual off";
        } else if (s_config && s_config->sched_mode == SCHED_MODE_SCHEDULED && active_rule >= 0) {
            reason = "Rule active";
        } else if (s_config && s_config->sched_mode == SCHED_MODE_SCHEDULED) {
            reason = "No active rule";
        }
    }

    bool ap_effective = ap_desired;
    bool sta_effective = sta_desired;
    bool tailscale_effective = tailscale_desired && sta_effective;
    bool safety_hold = false;
    if (!ap_desired || !sta_desired) {
        wifi_status_t wifi = {0};
        wifi_manager_get_status(&wifi);
        if (!ap_desired && !wifi.sta_connected) {
            ap_effective = true;
            safety_hold = true;
            reason = "Safety hold: STA offline";
        }
        if (!sta_desired) {
            ap_effective = true;
            tailscale_effective = false;
            reason = "STA off, AP kept for management";
        }
    }

    strlcpy(s_last_reason, reason, sizeof(s_last_reason));
    s_last_ap_desired = ap_desired;
    s_last_ap_effective = ap_effective;
    s_last_sta_desired = sta_desired;
    s_last_sta_effective = sta_effective;
    s_last_tailscale_desired = tailscale_desired;
    s_last_tailscale_effective = tailscale_effective;
    s_last_safety_hold = safety_hold;
    s_last_active_rule = active_rule;
    scheduler_unlock();

    esp_err_t ret = wifi_manager_set_ap_enabled(ap_effective);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to apply scheduled AP state: %s", esp_err_to_name(ret));
    }
    ret = wifi_manager_set_sta_scheduler_enabled(sta_effective);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to apply scheduled STA state: %s", esp_err_to_name(ret));
    }
    ret = tailscale_manager_set_scheduler_enabled(tailscale_effective);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to apply scheduled Tailscale state: %s", esp_err_to_name(ret));
    }
}

static void scheduler_task(void *arg)
{
    (void)arg;
    while (true) {
        scheduler_evaluate();
        vTaskDelay(pdMS_TO_TICKS(15000));
    }
}

esp_err_t scheduler_manager_init(repeater_config_t *config)
{
    s_config = config;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    scheduler_lock();
    apply_timezone_locked();
    start_sntp_locked();
    scheduler_unlock();

    BaseType_t ret = xTaskCreate(scheduler_task, "scheduler", 4096, NULL, 4, NULL);
    return ret == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t scheduler_manager_apply_config(repeater_config_t *config)
{
    scheduler_lock();
    s_config = config;
    apply_timezone_locked();
    if (s_ntp_started) {
        esp_sntp_restart();
    } else {
        start_sntp_locked();
    }
    scheduler_unlock();
    scheduler_evaluate();
    return ESP_OK;
}

esp_err_t scheduler_manager_sync_now(void)
{
    scheduler_lock();
    if (!s_ntp_started) {
        start_sntp_locked();
    } else {
        esp_sntp_restart();
    }
    scheduler_unlock();
    return ESP_OK;
}

void scheduler_manager_get_status(scheduler_status_t *status)
{
    memset(status, 0, sizeof(*status));
    scheduler_lock();
    time_t now = time(NULL);
    bool valid = time_is_valid();
    uint32_t next_s = 0;
    int next_rule = -1;
    time_t next_time = 0;

    status->time_valid = valid;
    status->ntp_synced = s_ntp_synced || esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED;
    status->ap_effective = s_last_ap_effective;
    status->ap_desired = s_last_ap_desired;
    status->sta_effective = s_last_sta_effective;
    status->sta_desired = s_last_sta_desired;
    status->tailscale_effective = s_last_tailscale_effective;
    status->tailscale_desired = s_last_tailscale_desired;
    status->safety_hold = s_last_safety_hold;
    status->mode = s_config ? s_config->sched_mode : SCHED_MODE_ALWAYS_ON;
    status->active_rule = s_last_active_rule;
    status->next_rule = -1;
    strlcpy(status->timezone, s_config ? s_config->sched_tz : CFG_SCHED_TZ_DEFAULT,
            sizeof(status->timezone));
    strlcpy(status->reason, s_last_reason, sizeof(status->reason));

    format_time(now, status->local_time, sizeof(status->local_time));
    find_next_change(now, &next_s, &next_rule, &next_time);
    status->next_change_s = next_s;
    status->next_rule = next_rule;
    if (next_time > 0) {
        format_time(next_time, status->next_change_local, sizeof(status->next_change_local));
    } else {
        strlcpy(status->next_change_local, "--", sizeof(status->next_change_local));
    }
    scheduler_unlock();
}
