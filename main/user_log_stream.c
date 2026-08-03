#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "user_log_stream.h"

static WebLogLine_t *g_log_history = NULL;
static uint32_t g_log_write_idx = 0;
static uint32_t g_log_seq_counter = 0;
static SemaphoreHandle_t g_log_mutex = NULL;
static vprintf_like_t g_original_vprintf = NULL;

int user_log_vprintf(const char *fmt, va_list args)
{
    int ret = 0;
    if (g_original_vprintf)
    {
        ret = g_original_vprintf(fmt, args);
    }
    else
    {
        ret = vprintf(fmt, args);
    }

    if (g_log_history != NULL && g_log_mutex != NULL)
    {
        if (xSemaphoreTake(g_log_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
        {
            WebLogLine_t *line = &g_log_history[g_log_write_idx];
            
            g_log_seq_counter++;
            line->seq = g_log_seq_counter;

            va_list args_copy;
            va_copy(args_copy, args);
            vsnprintf(line->text, LOG_LINE_MAX_LEN, fmt, args_copy);
            va_end(args_copy);

            int len = strlen(line->text);
            while (len > 0 && (line->text[len - 1] == '\n' || line->text[len - 1] == '\r'))
            {
                line->text[len - 1] = '\0';
                len--;
            }

            g_log_write_idx = (g_log_write_idx + 1) % LOG_LINE_MAX_COUNT;

            xSemaphoreGive(g_log_mutex);
        }
    }
    return ret;
}

void User_Log_Stream_Init(void)
{
    g_log_mutex = xSemaphoreCreateMutex();
    if (g_log_mutex == NULL)
    {
        return;
    }

    g_log_history = (WebLogLine_t *)heap_caps_malloc(LOG_LINE_MAX_COUNT * sizeof(WebLogLine_t), MALLOC_CAP_SPIRAM);
    if (g_log_history == NULL)
    {
        return;
    }
    memset(g_log_history, 0, LOG_LINE_MAX_COUNT * sizeof(WebLogLine_t));

    g_original_vprintf = esp_log_set_vprintf(user_log_vprintf);
}

uint32_t User_Log_Stream_Get(uint32_t last_seq, WebLogLine_t *out_logs, uint32_t max_count)
{
    if (g_log_history == NULL || g_log_mutex == NULL || out_logs == NULL || max_count == 0)
    {
        return 0;
    }

    uint32_t count = 0;
    if (xSemaphoreTake(g_log_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        for (uint32_t i = 0; i < LOG_LINE_MAX_COUNT; i++)
        {
            uint32_t idx = (g_log_write_idx + i) % LOG_LINE_MAX_COUNT;
            WebLogLine_t *line = &g_log_history[idx];
            
            if (line->seq > last_seq && line->text[0] != '\0')
            {
                memcpy(&out_logs[count], line, sizeof(WebLogLine_t));
                count++;
                if (count >= max_count)
                {
                    break;
                }
            }
        }
        xSemaphoreGive(g_log_mutex);
    }
    return count;
}
