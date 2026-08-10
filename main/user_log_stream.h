#ifndef USER_LOG_STREAM_H
#define USER_LOG_STREAM_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define LOG_LINE_MAX_COUNT  256
#define LOG_LINE_MAX_LEN    2048

typedef struct
{
    uint32_t seq;
    char text[LOG_LINE_MAX_LEN];
} WebLogLine_t;

/**
 * @brief Initialize log stream buffer in PSRAM and register vprintf interceptor.
 */
void User_Log_Stream_Init(void);

/**
 * @brief Read logs newer than a given sequence number.
 * @param last_seq The last sequence number the client has seen.
 * @param out_logs Buffer to fill with logs.
 * @param max_count Maximum number of logs to retrieve.
 * @return Number of log lines retrieved.
 */
uint32_t User_Log_Stream_Get(uint32_t last_seq, WebLogLine_t *out_logs, uint32_t max_count);

#endif // USER_LOG_STREAM_H
