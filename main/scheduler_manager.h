#pragma once

#include "config_storage.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#define SCHED_TIME_STR_LEN 32
#define SCHED_REASON_LEN 48

typedef struct {
    bool time_valid;
    bool ntp_synced;
    bool ap_effective;
    bool ap_desired;
    bool sta_effective;
    bool sta_desired;
    bool tailscale_effective;
    bool tailscale_desired;
    bool safety_hold;
    scheduler_mode_t mode;
    int active_rule;
    int next_rule;
    uint32_t next_change_s;
    char timezone[CFG_SCHED_TZ_LEN];
    char local_time[SCHED_TIME_STR_LEN];
    char next_change_local[SCHED_TIME_STR_LEN];
    char reason[SCHED_REASON_LEN];
} scheduler_status_t;

esp_err_t scheduler_manager_init(repeater_config_t *config);
esp_err_t scheduler_manager_apply_config(repeater_config_t *config);
esp_err_t scheduler_manager_sync_now(void);
void scheduler_manager_get_status(scheduler_status_t *status);
