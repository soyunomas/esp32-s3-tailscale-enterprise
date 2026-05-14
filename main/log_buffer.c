#include <string.h>
#include <stdio.h>
#include "log_buffer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* Static DRAM ring buffer. Avoid PSRAM here: the vprintf hook runs from any
 * task context (including WiFi/lwIP) and PSRAM access in some of those paths
 * caused crashes during Noise handshake. 16 KB DRAM is enough to capture a
 * full Tailscale connect cycle (~10 s) without rotating. */
static char s_buf[LOG_BUFFER_SIZE];
static size_t s_head = 0;   // next write position
static size_t s_count = 0;  // bytes stored (max LOG_BUFFER_SIZE)
static SemaphoreHandle_t s_mutex = NULL;
static vprintf_like_t s_orig_vprintf = NULL;

static int log_vprintf_hook(const char *fmt, va_list args)
{
    // Always forward to original output (UART)
    int ret = s_orig_vprintf(fmt, args);

    // Format into temp buffer
    char tmp[256];
    va_list args_copy;
    va_copy(args_copy, args);
    int len = vsnprintf(tmp, sizeof(tmp), fmt, args_copy);
    va_end(args_copy);
    if (len <= 0) return ret;
    if ((size_t)len >= sizeof(tmp)) len = sizeof(tmp) - 1;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        for (int i = 0; i < len; i++) {
            s_buf[s_head] = tmp[i];
            s_head = (s_head + 1) % LOG_BUFFER_SIZE;
        }
        s_count += len;
        if (s_count > LOG_BUFFER_SIZE) s_count = LOG_BUFFER_SIZE;
        xSemaphoreGive(s_mutex);
    }

    return ret;
}

esp_err_t log_buffer_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    s_orig_vprintf = esp_log_set_vprintf(log_vprintf_hook);
    return ESP_OK;
}

size_t log_buffer_read(char *dst, size_t dst_size)
{
    if (!dst || dst_size == 0 || !s_mutex) return 0;

    size_t copied = 0;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        size_t avail = (s_count < LOG_BUFFER_SIZE) ? s_count : LOG_BUFFER_SIZE;
        size_t to_copy = (avail < dst_size - 1) ? avail : dst_size - 1;

        // Start reading from oldest byte
        size_t start;
        if (s_count >= LOG_BUFFER_SIZE) {
            start = s_head;  // oldest data is right after head
        } else {
            start = 0;
        }

        for (size_t i = 0; i < to_copy; i++) {
            dst[i] = s_buf[(start + i) % LOG_BUFFER_SIZE];
        }
        dst[to_copy] = '\0';
        copied = to_copy;

        xSemaphoreGive(s_mutex);
    }
    return copied;
}
