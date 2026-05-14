#pragma once

#include "config_storage.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#define TAILSCALE_VPN_IP_LEN 16
#define TAILSCALE_ROUTE_STATE_LEN 20

typedef enum {
    TAILSCALE_STATE_DISABLED = 0,
    TAILSCALE_STATE_NOT_CONFIGURED,
    TAILSCALE_STATE_WAITING_FOR_WIFI,
    TAILSCALE_STATE_STARTING,
    TAILSCALE_STATE_CONNECTING,
    TAILSCALE_STATE_CONNECTED,
    TAILSCALE_STATE_RECONNECTING,
    TAILSCALE_STATE_ERROR,
} tailscale_state_t;

typedef struct {
    bool available;
    bool enabled;
    bool scheduler_enabled;
    bool configured;
    bool started;
    bool connected;
    tailscale_state_t state;
    char vpn_ip[TAILSCALE_VPN_IP_LEN];
    bool subnet_route_advertised;
    bool subnet_route_active;
    char advertised_cidr[CFG_TS_CTRL_HOST_LEN];
    char subnet_route_state[TAILSCALE_ROUTE_STATE_LEN];
    uint8_t peer_count;
    esp_err_t last_error;
    uint32_t heap_internal_free;
    uint32_t heap_psram_free;
} tailscale_status_t;

esp_err_t tailscale_manager_init(const repeater_config_t *config);
esp_err_t tailscale_manager_apply_config(const repeater_config_t *config);
esp_err_t tailscale_manager_set_scheduler_enabled(bool enabled);
void tailscale_manager_on_sta_got_ip(void);
void tailscale_manager_on_sta_disconnected(void);
void tailscale_manager_get_status(tailscale_status_t *status);
const char *tailscale_manager_state_to_string(tailscale_state_t state);
