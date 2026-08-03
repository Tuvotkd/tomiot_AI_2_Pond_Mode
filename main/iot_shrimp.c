#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"

#include "lwip/err.h"
#include "lwip/sys.h"
#include "cJSON.h"
#include "sys/socket.h"
#include "tcp_server_com.h"
#include "user_system.h"
#include "user_ouput.h"
#include "user_time.h"
#include "user_console.h"
#include "driver/uart.h"
#include "esp_vfs_dev.h"
#include "user_storage.h"
#include "user_ota.h"
#include "user_fram.h"
#include "user_external_flash.h"
#include "user_azure.h"
#include "user_http_server.h"
#include "wifi_config_manager.h"
#include "RS485.h"
#include "user_log_stream.h"

TaskHandle_t Tcp_Task_Handle;
TaskHandle_t IO_Task_Handle;
TaskHandle_t Timer_Task_Handle;
TaskHandle_t Console_Task_Handle;
TaskHandle_t Storage_Task_Handle;
TaskHandle_t OTA_Task_Handle;
TaskHandle_t Fram_Task_Handle;
TaskHandle_t User_Input_Task_Handle;
TaskHandle_t Azure_Task_Handle;
TaskHandle_t Http_Server_Task_Handle;
TaskHandle_t Ext_Flash_Task_Handle;

extern EventGroupHandle_t IO_Event_Group;


void Tcp_Task(void *pvParameters);
void IO_Task(void *pvParameters);

void Tcp_Task(void *pvParameters)
{
    Tcp_Server_Communication_Task(&TCP_Handle);
}

void IO_Task(void *pvParameters)
{
    IO_Driver_Task();
}

void Timer_Task(void *pvParameters)
{
    User_Time_Task();
}

void Console_Task(void *pvParameters)
{
    User_Console_Task();
}

void Storage_Task(void *pvParameters)
{
    Nvs_Storage_Task();
}

void OTA_Task(void *pvParameters)
{
    User_Ota_Task();
}

void Fram_Task(void *pvParameters)
{
    User_Fram_Task();
}

void Azure_Task(void *pvParameters)
{
    User_Azure_Task();
}

void Http_Server_Task(void *pvParameters)
{
    User_Http_Server_Task();
}

void Ext_Flash_Task(void *pvParameters)
{
    User_External_Flash_Task(pvParameters);
}

static const char *TAG = "wifi station";

int dummy_vprintf(const char *fmt, va_list args)
{
    // Do nothing
    return 0;
}


void app_main(void)
{
    // Khởi tạo bộ đệm ghi log thời gian thực vào PSRAM
    User_Log_Stream_Init();

    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    User_Input_IO_Config();
    User_System_Init();
    if (Sys_Info.vfdEnabled == 1)
    {
        RS485_Init();
    }

    // Khởi tạo SPI2 và nạp File System từ Flash ngoại vi Winbond W25Q128
    Fram_Init();
    User_External_Flash_Init();
    User_External_Flash_Mount();

    if(User_Input_Check_Is_Active(IO_CONFIG_INPUT_1) == true)
    {
        sys_delay_ms(2000);
        ESP_LOGI("IO INPUT", "Input config number 1 is actived, jump to config mode");
        sys_delay_ms(1000);

        esp_log_level_set("*", ESP_LOG_NONE);
        esp_vfs_dev_uart_use_driver(UART_NUM_0);
        uart_driver_delete(UART_NUM_0);

        if(xTaskCreatePinnedToCore(Console_Task, "Console Task", 3*4096, NULL, 2, &Console_Task_Handle, 1) == pdPASS)
        {
            printf("Create console task\n\n\n");
        }
        else printf("Create console task fail\n\n\n");
    }
    else
    {

        // if(xTaskCreatePinnedToCore(Storage_Task, "Storage Task", 3*4096, NULL, 2, &Storage_Task_Handle, 1) == pdPASS)
        // {
        //     printf("Create console task\n\n\n");
        // }else{
        //     printf("Create console task fail\n\n\n");
        // }

        if (CONFIG_LOG_MAXIMUM_LEVEL > CONFIG_LOG_DEFAULT_LEVEL) 
        {
            /* If you only want to open more logs in the wifi module, you need to make the max level greater than the default level,
            * and call esp_log_level_set() before esp_wifi_init() to improve the log level of the wifi module. */
            esp_log_level_set("wifi", CONFIG_LOG_MAXIMUM_LEVEL);
        }

        // ─── Ưu tiên khởi tạo IO_Task ngay lập tức để xử lý phần cứng & khôi phục trạng thái FRAM ───
        IO_Event_Group = xEventGroupCreate();
        if(xTaskCreatePinnedToCore(IO_Task, "IO Task", 8192, NULL, 5, &IO_Task_Handle, 1) == pdPASS)
        {
            ESP_LOGI(TAG, "Create IO task successfully\n");
        }
        else ESP_LOGI(TAG, "Create IO task fail\n");

        // ─── LỚP 4: Đăng ký IO Task vào Task Watchdog Timer (120 giây) ───
        // Nếu IO Task bị treo (deadlock, FRAM hang, v.v.) mà không feed WDT
        // trong 120 giây, ESP-IDF sẽ tự động gây panic và restart.
        esp_task_wdt_config_t wdt_config =
        {
            .timeout_ms    = 120000U,  // 120 giây
            .idle_core_mask = 0,       // Không monitor idle task
            .trigger_panic  = true,    // Gây panic → restart khi timeout
        };
        if(esp_task_wdt_reconfigure(&wdt_config) == ESP_OK)
        {
            if(IO_Task_Handle != NULL)
            {
                esp_err_t wdt_err = esp_task_wdt_add(IO_Task_Handle);
                if(wdt_err == ESP_OK)
                {
                    ESP_LOGI(TAG, "Task WDT registered for IO Task (120s timeout)");
                }
                else ESP_LOGW(TAG, "Task WDT register failed: %s", esp_err_to_name(wdt_err));
            }
        }
        else ESP_LOGW(TAG, "Task WDT reconfigure failed, watchdog not active");

        ESP_LOGI(TAG, "ESP_WIFI_MODE_APSTA");
        wifi_config_manager_init();

        if(xTaskCreatePinnedToCore(Http_Server_Task, "http server Task", 5*4096, NULL, 5, &Http_Server_Task_Handle, 0) == pdPASS)
        {
            printf("Create http server task\n\n\n");
        }
        else printf("Create http server task fail\n\n\n");

        while (!Sys_Info.isWifiConnected)
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        vTaskDelay(pdMS_TO_TICKS(2000));

        if(xTaskCreatePinnedToCore(Timer_Task, "Timer Task", 4096, NULL, 2, &Timer_Task_Handle, 1) == pdPASS)
        {
            ESP_LOGI(TAG, "Create timer task successfully\n");
        }
        else ESP_LOGI(TAG, "Create timer task fail\n");
        
        while(!Sys_Info.isTimeSync)
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        vTaskDelay(pdMS_TO_TICKS(2000)); 
        // if(xTaskCreatePinnedToCore(Tcp_Task, "TCP Task", 5*4096, NULL, 5, &Tcp_Task_Handle, 0) == pdPASS)
        // {
        //     ESP_LOGI(TAG, "Create TCP task successfully\n");
        // }else{
        //     ESP_LOGI(TAG, "Create TCP task fail\n");
        // }


        // if(xTaskCreatePinnedToCore(Storage_Task, "Storage Task", 3*4096, NULL, 2, &Storage_Task_Handle, 1) == pdPASS)
        // {
        //     printf("Create console task\n\n\n");
        // }else{
        //     printf("Create console task fail\n\n\n");
        // }

        // ESP_LOGI("FIRMWARE", "Version: %s", VERSION);

        if(xTaskCreatePinnedToCore(OTA_Task, "OTA Task", 8192, NULL, 5, &OTA_Task_Handle, 1) == pdPASS)
        {
            ESP_LOGI("MAIN", "Create OTA task\n");
        }
        else ESP_LOGE("MAIN", "Create OTA task fail\n");

        // if(xTaskCreatePinnedToCore(Fram_Task, "OTA Task", 3*4096, NULL, 2, &Fram_Task_Handle, 1) == pdPASS)
        // {
        //     printf("Create Fram task\n\n\n");
        // }else{
        //     printf("Create Fram task fail\n\n\n");
        // ?XCXd8IzHEv4qq2xbk54pfTRqjGgmCdUN2AIoTNYnKrI=

        if(xTaskCreatePinnedToCore(Azure_Task, "Azure Task", 4*4096, NULL, 5, &Azure_Task_Handle, 0) == pdPASS)
        {
            ESP_LOGI(TAG, "Create Azure task successfully\n");
        }
        else ESP_LOGE(TAG, "Create Azure task fail\n");

        if(xTaskCreatePinnedToCore(Ext_Flash_Task, "Ext Flash Task", 8192, NULL, 3, &Ext_Flash_Task_Handle, 1) == pdPASS)
        {
            ESP_LOGI(TAG, "Create External Flash Task successfully\n");
        }
        else ESP_LOGE(TAG, "Create External Flash Task fail\n");  
    }
    
}
