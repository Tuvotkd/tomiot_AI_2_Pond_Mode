#include "user_ota.h"
#include "esp_https_ota.h"
#include "freertos/FreeRTOS.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include <string.h>

EventGroupHandle_t otaEventGroup;
static bool s_ota_use_auth_header = true;

esp_err_t IRAM_ATTR _http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_CONNECTED)
    {
        if (s_ota_use_auth_header)
        {
            // esp_http_client_set_header(evt->client, "Authorization", "Bearer your_token");
            esp_http_client_set_header(evt->client, "Authorization", "Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJUZW5hbnRDb2RlIjoicHZvaWwiLCJodHRwOi8vc2NoZW1hcy5taWNyb3NvZnQuY29tL3dzLzIwMDgvMDYvaWRlbnRpdHkvY2xhaW1zL3JvbGUiOiJEZXZpY2UiLCJVc2VyTmFtZSI6InBlY28iLCJuYmYiOjE2NDQ1NTIxOTcsImV4cCI6MTcwNzY2NjAxNywiaXNzIjoiaHR0cDovL3NtYXJ0cGV0cm8uaW8vIiwiYXVkIjoiU21hcnRQZXRybyJ9.03hQ3zdz3YJO-y8lfYV805qhapYts1iwdHkwVR-skms");
        }
    }
    return ESP_OK;
}


static bool prvBuildOtaUrl(const char *updateFileName, char *out, size_t out_len)
{
    if(updateFileName == NULL || out == NULL || out_len == 0)
    {
        return false;
    }

    static const char *base_url = "https://shrimpiotdblobs.blob.core.windows.net";

    if(updateFileName[0] == '/')
    {
        return (snprintf(out, out_len, "%s%s", base_url, updateFileName) > 0);
    }

    return (snprintf(out, out_len, "%s/%s", base_url, updateFileName) > 0);
}

esp_err_t update_firmware(const char *updateFileName)
{
    if(updateFileName == NULL)
    {
        return ESP_FAIL;
    }

    char updateUrl[256] = {0};
    if((strncmp(updateFileName, "http://", 7) == 0) || (strncmp(updateFileName, "https://", 8) == 0))
    {
        if(snprintf(updateUrl, sizeof(updateUrl), "%s", updateFileName) <= 0)
        {
            return ESP_FAIL;
        }
    }
    else if(!prvBuildOtaUrl(updateFileName, updateUrl, sizeof(updateUrl)))
    {
        return ESP_FAIL;
    }

    printf("URL: %s\n", updateUrl);
    s_ota_use_auth_header = (strstr(updateUrl, "sig=") == NULL);

    esp_http_client_config_t config = {0}; 

    config.url = updateUrl;
    config.event_handler = _http_event_handler;
    config.cert_pem = NULL;
    config.buffer_size_tx = 1024;

    esp_https_ota_config_t ota_config =
    {
        .http_config = &config,
    };

    
    ESP_LOGI("OTA", "Attempting to download update from %s", config.url);
    esp_err_t ret = esp_https_ota(&ota_config);
    if (ret == ESP_OK)
    {
        ESP_LOGI("OTA", "OTA Succeed, Rebooting...");
        esp_restart();
    } 
    else
    {
        ESP_LOGE("OTA", "Firmware upgrade failed");
        ret = ESP_FAIL;
    }

    return ret;
}



char g_ota_update_url[256] = {0};

void User_Ota_Task(void)
{
    otaEventGroup = xEventGroupCreate();

    while(1)
    {
        EventBits_t otaWaitBits = xEventGroupWaitBits(otaEventGroup, OTA_WAIT_BIT, pdTRUE, pdFALSE, portMAX_DELAY);

        if(otaWaitBits & OTA_WAIT_BIT)
        {
            ESP_LOGI("OTA", "OTA request received. Waiting 3s for Azure to clean up Heap...");
            vTaskDelay(pdMS_TO_TICKS(3000));
            
            ESP_LOGI("OTA", "Activating firmware download via HTTPS: %s", g_ota_update_url);
            esp_err_t ret = update_firmware(g_ota_update_url);
            if(ret == ESP_OK)
            {
                ESP_LOGI("OTA", "OTA Succeed, Rebooting...");
                esp_restart();
            }
            else
            {
                ESP_LOGE("OTA", "Firmware upgrade failed! Rebooting.");
                esp_restart();
            }
        }
    }
}


