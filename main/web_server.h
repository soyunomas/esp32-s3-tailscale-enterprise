#pragma once

#include "esp_err.h"
#include "config_storage.h"

esp_err_t web_server_start(repeater_config_t *config);
void web_server_stop(void);
