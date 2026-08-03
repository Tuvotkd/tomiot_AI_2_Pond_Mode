#include "user_external_flash.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_flash_spi_init.h"
#include "esp_partition.h"
#include "esp_spiffs.h"
#include "user_fram.h" // For SPI_HOST definition

#include <time.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define AZURE_LOG_QUEUE_SIZE 10
#define AZURE_LOG_PAYLOAD_MAX 2048

typedef struct
{
    char payload[AZURE_LOG_PAYLOAD_MAX];
} azure_log_item_t;

static QueueHandle_t s_azure_log_queue = NULL;

static const char *TAG = "EXT_FLASH";

static esp_flash_t *ext_flash_chip = NULL;
static const esp_partition_t *ext_partition = NULL;
static bool s_mounted = false;

esp_err_t User_External_Flash_Init(void)
{
    // The SPI2 bus (SPI_HOST) must already be initialized by Fram_Init() before calling this.
    // We add the external Winbond Flash device to the existing bus.
    const esp_flash_spi_device_config_t dev_config =
    {
        .host_id = SPI_HOST,
        .cs_id = 0,
        .cs_io_num = PIN_NUM_FLASH_CS,
        .io_mode = SPI_FLASH_DIO,
        .freq_mhz = 10, // 10 MHz
    };

    esp_err_t err = spi_bus_add_flash_device(&ext_flash_chip, &dev_config);
    if(err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to add SPI flash device to bus: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_flash_init(ext_flash_chip);
    if(err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize SPI flash chip: %s", esp_err_to_name(err));
        return err;
    }

    uint32_t flash_size = 0;
    esp_flash_get_size(ext_flash_chip, &flash_size);
    ESP_LOGI(TAG, "External Flash chip found! Capacity: %lu bytes (%lu MB)", flash_size, flash_size / (1024 * 1024));

    // Default SPIFFS uses 256-byte page size with 16-bit page index (max 65535 pages = ~15.99 MB).
    // A 16 MB partition has exactly 65536 pages, which overflows the 16-bit page index and fails with ESP_ERR_INVALID_ARG.
    // We cap partition size to 15.5 MB (16,252,928 bytes = 63,488 pages) so it fits safely under 65535 pages.
    uint32_t partition_size = flash_size;
    if (partition_size > (15 * 1024 * 1024 + 512 * 1024))
    {
        partition_size = 15 * 1024 * 1024 + 512 * 1024; // 15.5 MB
    }

    // Register external flash partition named "storage"
    err = esp_partition_register_external(ext_flash_chip, 0, partition_size, "storage", ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, &ext_partition);
    if(err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to register external partition: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "External partition 'storage' (%lu KB) successfully registered!", partition_size / 1024);
    return ESP_OK;
}

esp_err_t User_External_Flash_Mount(void)
{
    if (esp_spiffs_mounted("storage"))
    {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Mounting SPIFFS filesystem at /web...");
    
    esp_vfs_spiffs_conf_t conf =
    {
        .base_path = "/web",
        .partition_label = "storage",
        .max_files = 16,
        .format_if_mount_failed = true // Automatically formats if unformatted
    };

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if(err != ESP_OK)
    {
        ESP_LOGW(TAG, "SPIFFS partition unformatted or mount failed: %s. Formatting Flash (this may take ~20-30s on 15.5MB chip)...", esp_err_to_name(err));
        esp_err_t fmt_err = esp_spiffs_format("storage");
        if (fmt_err == ESP_OK)
        {
            ESP_LOGI(TAG, "Formatting completed successfully! Retrying SPIFFS mount...");
            err = esp_vfs_spiffs_register(&conf);
        }
        else
        {
            ESP_LOGE(TAG, "Formatting failed: %s", esp_err_to_name(fmt_err));
        }
    }

    if(err != ESP_OK)
    {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(err));
        return err;
    }

    s_mounted = true;

    // Check spiffs partition usage
    size_t total = 0, used = 0;
    err = esp_spiffs_info("storage", &total, &used);
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "SPIFFS partition size: %d KB, used: %d KB", total / 1024, used / 1024);
    }

    ESP_LOGI(TAG, "SPIFFS filesystem mounted successfully at /web!");
    return ESP_OK;
}

esp_err_t User_External_Flash_Format(void)
{
    ESP_LOGI(TAG, "Formatting SPIFFS partition 'storage'...");
    esp_err_t err = esp_spiffs_format("storage");
    if(err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to format SPIFFS partition: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "Formatted SPIFFS partition successfully!");
    return ESP_OK;
}

esp_err_t User_External_Flash_Log_Azure_Command(const char *raw_json_payload)
{
    if (!raw_json_payload || raw_json_payload[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (s_azure_log_queue == NULL) return ESP_ERR_INVALID_STATE;

    azure_log_item_t item;
    memset(&item, 0, sizeof(item));
    strncpy(item.payload, raw_json_payload, sizeof(item.payload) - 1);

    if (xQueueSend(s_azure_log_queue, &item, pdMS_TO_TICKS(100)) == pdPASS)
    {
        return ESP_OK;
    }
    else
    {
        ESP_LOGW(TAG, "Log queue full, dropping Azure command log");
        return ESP_ERR_TIMEOUT;
    }
}

static void prv_rotate_7day_history_logs(time_t now_epoch)
{
    DIR *dir = opendir("/web");
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL)
    {
        if (strncmp(entry->d_name, "hist_", 5) == 0 && strstr(entry->d_name, ".txt"))
        {
            int year = 0, month = 0, day = 0;
            if (sscanf(entry->d_name, "hist_%d_%d_%d.txt", &year, &month, &day) == 3)
            {
                struct tm tm_file = {0};
                tm_file.tm_year = year - 1900;
                tm_file.tm_mon  = month - 1;
                tm_file.tm_mday = day;
                tm_file.tm_isdst = -1;

                time_t file_epoch = mktime(&tm_file);
                if (file_epoch != (time_t)-1)
                {
                    if ((now_epoch - file_epoch) > (7 * 86400))
                    {
                        char old_filepath[300];
                        snprintf(old_filepath, sizeof(old_filepath), "/web/%s", entry->d_name);
                        if (unlink(old_filepath) == 0)
                        {
                            ESP_LOGI(TAG, "7-Day Rotation: Removed old history log file '%s'", entry->d_name);
                        }
                    }
                }
            }
        }
    }
    closedir(dir);
}

void User_External_Flash_Task(void *pvParameters)
{
    (void)pvParameters;

    s_azure_log_queue = xQueueCreate(AZURE_LOG_QUEUE_SIZE, sizeof(azure_log_item_t));
    if (s_azure_log_queue == NULL)
    {
        ESP_LOGE(TAG, "Failed to create Azure log queue");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "External_Flash_Task started! Listening for Azure commands & managing 7-day log rotation...");

    azure_log_item_t item;
    TickType_t last_rotation_check = 0;

    while (1)
    {
        if (xQueueReceive(s_azure_log_queue, &item, pdMS_TO_TICKS(1000)) == pdTRUE)
        {
            if (!esp_spiffs_mounted("storage"))
            {
                ESP_LOGW(TAG, "SPIFFS not mounted yet! Attempting mount...");
                User_External_Flash_Mount();
            }

            time_t now;
            time(&now);
            struct tm timeinfo;
            localtime_r(&now, &timeinfo);

            char date_str[32];
            char log_filepath[300];

            if (timeinfo.tm_year + 1900 >= 2024)
            {
                snprintf(date_str, sizeof(date_str), "%04d_%02d_%02d",
                         timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);
                snprintf(log_filepath, sizeof(log_filepath), "/web/hist_%s.txt", date_str);
            }
            else
            {
                snprintf(log_filepath, sizeof(log_filepath), "/web/hist_latest.txt");
            }

            if (!User_External_Flash_File_Exists(log_filepath))
            {
                FILE *create_f = fopen(log_filepath, "w");
                if (create_f != NULL)
                {
                    fclose(create_f);
                }
            }

            FILE *f = fopen(log_filepath, "a");
            if (f == NULL)
            {
                f = fopen(log_filepath, "w");
            }

            if (f != NULL)
            {
                if (timeinfo.tm_year + 1900 >= 2024)
                {
                    fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d] %s\n",
                            timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                            timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec,
                            item.payload);
                }
                else
                {
                    fprintf(f, "[NO_TIME_SYNC] %s\n", item.payload);
                }
                fflush(f);
                fclose(f);
                ESP_LOGI(TAG, "Azure command logged to Flash: %s", log_filepath);
            }
            else
            {
                ESP_LOGE(TAG, "Failed to open log file '%s' for writing (errno: %d - %s)",
                         log_filepath, errno, strerror(errno));
            }

            if (timeinfo.tm_year + 1900 >= 2024)
            {
                prv_rotate_7day_history_logs(now);
            }
        }

        if (xTaskGetTickCount() - last_rotation_check > pdMS_TO_TICKS(3600000))
        {
            last_rotation_check = xTaskGetTickCount();
            time_t now;
            time(&now);
            struct tm timeinfo;
            localtime_r(&now, &timeinfo);
            if (timeinfo.tm_year + 1900 >= 2024)
            {
                prv_rotate_7day_history_logs(now);
            }
        }
    }
}

bool User_External_Flash_File_Exists(const char *path)
{
    struct stat st;
    if(stat(path, &st) == 0)
    {
        return true;
    }
    return false;
}

#define EVENT_LOG_FILEPATH "/web/Wifi_Azure_History.csv"
#define MAX_EVENT_LOG_SIZE (256 * 1024) // 256 KB limit

esp_err_t User_External_Flash_Log_Event(const char *event_type, const char *details)
{
    if (event_type == NULL) return ESP_ERR_INVALID_ARG;
    if (details == NULL) details = "";

    if (!s_mounted)
    {
        ESP_LOGW(TAG, "External Flash not mounted, skipping log event");
        return ESP_ERR_INVALID_STATE;
    }

    struct stat st;
    bool is_new_file = (stat(EVENT_LOG_FILEPATH, &st) != 0);

    // If file size exceeds 256 KB, truncate to prevent Flash overflow
    if (!is_new_file && st.st_size > MAX_EVENT_LOG_SIZE)
    {
        ESP_LOGW(TAG, "Log file %s exceeded 256KB limit (%ld bytes). Truncating...", EVENT_LOG_FILEPATH, (long)st.st_size);
        FILE *f_trunc = fopen(EVENT_LOG_FILEPATH, "w");
        if (f_trunc)
        {
            fprintf(f_trunc, "Time,Event,Detail\n");
            fclose(f_trunc);
        }
        is_new_file = false;
    }

    FILE *f = fopen(EVENT_LOG_FILEPATH, "a");
    if (!f)
    {
        ESP_LOGE(TAG, "Failed to open %s for appending", EVENT_LOG_FILEPATH);
        return ESP_FAIL;
    }

    // Write CSV Header if newly created
    if (is_new_file)
    {
        fprintf(f, "Time,Event,Detail\n");
    }

    // Format timestamp
    time_t now;
    time(&now);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    char time_str[32];
    if (timeinfo.tm_year + 1900 >= 2024)
    {
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &timeinfo);
    }
    else
    {
        snprintf(time_str, sizeof(time_str), "BootSec: %lu", (unsigned long)(xTaskGetTickCount() * portTICK_PERIOD_MS / 1000));
    }

    fprintf(f, "%s,%s,%s\n", time_str, event_type, details);
    fflush(f);
    fclose(f);

    ESP_LOGI(TAG, "Network Event Logged: [%s] %s - %s", time_str, event_type, details);
    return ESP_OK;
}
