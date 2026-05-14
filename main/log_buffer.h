#pragma once

#include "esp_err.h"
#include <stddef.h>

/* 32 KB ring buffer. Allocated from PSRAM at init() to avoid DRAM pressure
 * (we have 8 MB free PSRAM on the S3 N16R8). Diagnostic Tailscale cycles
 * easily produce 5-10 KB per attempt, so 4 KB rotates too fast. */
#define LOG_BUFFER_SIZE 32768

esp_err_t log_buffer_init(void);
size_t log_buffer_read(char *dst, size_t dst_size);
