#include "user_storage.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_err.h"
#include "tcp_server_com.h"
#include "user_system.h"


#define STORAGE_NAMESPACE "storage"

nvs_handle_t nvsHandle;


bool Nvs_Write_String(const char *key, const char *value)
{
    esp_err_t err = ESP_OK;
    bool result = false;

    err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &nvsHandle);
    if(err != ESP_OK)
    {
        printf("NVS WRITE: Error (%s) opening NVS handle!\n", esp_err_to_name(err));
    }else{
        // printf("NVS WRITE: NVS handle opened successfully\n");

        err = nvs_set_str(nvsHandle, key, value);
        if(err != ESP_OK)
        {
            printf("NVS WRITE: Set str fail\n");
        }else{
            result = true;
            // printf("NVS WRITE: NVS handle saved successfully\n\n");
        }
    }

    nvs_close(nvsHandle);
    return result;
}

bool Nvs_Write_Number(const char *key, uint32_t value)
{
    esp_err_t err = ESP_OK;
    bool result = false;

    err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &nvsHandle);
    if(err != ESP_OK)
    {
        printf("NVS WRITE: Error (%s) opening NVS handle!\n", esp_err_to_name(err));
    }else{
        // printf("NVS WRITE: NVS handle opened successfully\n");

        err = nvs_set_u32(nvsHandle, key, value);
        if(err != ESP_OK)
        {
            printf("NVS WRITE: Set number fail\n");
        }else{
            result = true;
            // printf("NVS WRITE: Set number successfully\n\n");
        }
    }
    

    nvs_close(nvsHandle);
    return result;
}

bool Nvs_Read_String(const char *key, char *value)
{
    esp_err_t err = ESP_OK;
    bool result = false;

    err = nvs_open(STORAGE_NAMESPACE, NVS_READONLY, &nvsHandle);
    if(err != ESP_OK)
    {
        printf("NVS READ: Error (%s) opening NVS handle!\n", esp_err_to_name(err));
    }else{
        // printf("NVS READ: NVS handle opened successfully and get size\n");

        size_t strSize = 0;
        err = nvs_get_str(nvsHandle, key, NULL, &strSize);

        if(err != ESP_OK)
        {
            printf("NVS READ: get size fail %s\n", esp_err_to_name(err));
        }else{

            // printf("NVS READ: NVS get size successfully, then read data\n\n");
            err = nvs_get_str(nvsHandle, key, value, &strSize);
            if (err == ESP_OK)
            {
                result = true;
                // printf("NVS READ: Get string done\n");
            } else {
                printf("NVS READ: Err %s\n", esp_err_to_name(err));
            }
            
        }
    }

    nvs_close(nvsHandle);

    return result;

}

bool Nvs_Read_Number(const char *key, uint32_t *value)
{
    esp_err_t err = ESP_OK;
    bool result = false;

    err = nvs_open(STORAGE_NAMESPACE, NVS_READONLY, &nvsHandle);
    if(err != ESP_OK)
    {
        printf("NVS READ: Error (%s) opening NVS handle!\n", esp_err_to_name(err));
    }
    else
    {
        err = nvs_get_u32(nvsHandle, key, value);

        if(err != ESP_OK)
        {
            printf("NVS READ: get number %s\n", esp_err_to_name(err));
        }
        else result = true;
    }

    nvs_close(nvsHandle);

    return result;

}



void Nvs_Storage_Task(void)
{
    while(1)
    {

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}










