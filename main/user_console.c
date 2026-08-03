#include "user_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "string.h"
#include "cJSON.h"
#include "tcp_server_com.h"
#include "user_storage.h"
#include "user_ota.h"
#include "user_azure.h"
#include "user_fram.h"
#include "user_ouput.h"
#include "user_system.h"

#define BUF_SIZE            1024
#define UART_PORT_NUM       UART_NUM_0

static QueueHandle_t uartQueue;

Console_Handle_t consoleHandle;
uart_event_t event;


void uart_init(void)
{
    const uart_config_t uartConfig =
    {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };

    // Install UART driver with RX buffer and event queue
    uart_driver_install(UART_NUM_0, BUF_SIZE * 2, BUF_SIZE * 2, 100, &uartQueue, 0);
    uart_param_config(UART_NUM_0, &uartConfig);
    uart_set_pin(UART_NUM_0, 43, 44, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    // Configure pattern detection (example: '\n')
    uart_enable_pattern_det_baud_intr(UART_NUM_0, '\n', 1, 9000, 0, 0);
    uart_set_rx_timeout(UART_NUM_0, 50);
    uart_pattern_queue_reset(UART_NUM_0, 100);  // queue size for detected patterns
}

void User_Console_Handle(char *pData)
{
    cJSON *parsed = cJSON_Parse(pData);

    if(parsed)
    {
        const cJSON *serverIp = cJSON_GetObjectItem(parsed, "ServerIp");
        if(serverIp)
        {
            if(cJSON_IsString(serverIp))
            {
                printf("CONSOLE: SERVER_IP=%s\n", serverIp->valuestring);
                if(strcmp(serverIp->valuestring, TCP_Handle.remoteIP) != 0)
                {
                    if(Nvs_Write_String(STORAGE_KEY_SEVER_IP, serverIp->valuestring) == false)
                    {
                        printf("NVS: Save SERVER_IP fail\n");
                    }
                    else
                    {
                        printf("NVS: SERVER_IP=%s\n", serverIp->valuestring);
                    }
                }
                else
                {
                    printf("NVS: No change\n");
                }
            }
        }

        const cJSON *serverPort = cJSON_GetObjectItem(parsed, "ServerPort");
        if(serverPort)
        {
            if(cJSON_IsNumber(serverPort))
            {
                printf("CONSOLE: SERVER_PORT=%d\n", serverPort->valueint);

                if(serverPort->valueint != TCP_Handle.remotePort)
                {
                    if(Nvs_Write_Number(STORAGE_KEY_SEVER_PORT, serverPort->valueint) == false)
                    {
                        printf("NVS: Save SERVER_PORT fail\n");
                    }
                    else
                    {
                        printf("NVS: SERVER_PORT=%d\n", serverPort->valueint);
                    }
                }
                else
                {
                    printf("NVS: No change\n");
                }
            }
        }

        const cJSON *ssid = cJSON_GetObjectItem(parsed, "Ssid");
        if(ssid)
        {
            if(cJSON_IsString(ssid))
            {
                printf("CONSOLE: SSID=%s\n", ssid->valuestring);
                if(strcmp(ssid->valuestring, TCP_Handle.ssid) != 0)
                {
                    if(Nvs_Write_String(STORAGE_KEY_WIFI_SSID, ssid->valuestring) == false)
                    {
                        printf("NVS: Save SSID fail\n");
                    }
                    else
                    {
                        printf("NVS: SSID=%s\n", ssid->valuestring);
                    }
                
                }
                else
                {
                    printf("NVS: No change\n");
                }
            }
        }

        const cJSON *pass = cJSON_GetObjectItem(parsed, "Pass");
        if(pass)
        {
            if(cJSON_IsString(pass))
            {
                printf("CONSOLE: PASS=%s\n", pass->valuestring);
                if(strcmp(pass->valuestring, TCP_Handle.pass) != 0)
                {
                    if(Nvs_Write_String(STORAGE_KEY_WIFI_PASS, pass->valuestring) == false)
                    {
                        printf("NVS: Save PASS fail\n");
                    }
                    else
                    {
                        printf("NVS: PASS=%s\n", pass->valuestring);
                    }
                }
                else
                {
                    printf("NVS: No change\n");
                }
            }
        }

        const cJSON *msg = cJSON_GetObjectItem(parsed, "Message");
        if(msg)
        {
            if(cJSON_IsString(msg))
            {
                // printf("CONSOLE: MSG=%s\n", msg->valuestring);
                if(strncmp(msg->valuestring, "RESETT", strlen("RESETT")) == 0)
                {
                    printf("CONSOLE: SYSTEM RESET\n");
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    esp_restart();
                }
                else if(strncmp(msg->valuestring, "CLEAR_FRAM", strlen("CLEAR_FRAM")) == 0)
                {
                    printf("CONSOLE: CLEAR ENTIRE FRAM\n");
                    FRAM_Delete_All();
                    memset(&DeviceHandle, 0, sizeof(DeviceHandle));

                    uint8_t test[32];

                    Fram_Read_Data(0x0000, test, 32);

                    for(int i=0;i<32;i++)
                    {
                        printf("%02X ", test[i]);
                    }
                }
                else if(strncmp(msg->valuestring, "CLEAR_RESET_COUNT", strlen("CLEAR_RESET_COUNT")) == 0)
                {
                    printf("CONSOLE: CLEAR RESET COUNT\n");
                    User_System_Clear_Reset_Count();
                }
            }

        }

        const cJSON *version = cJSON_GetObjectItem(parsed, "Ver");
        if(version)
        {
            if(cJSON_IsString(version))
            {
                char verOTA[16] = {0};
                strcpy(verOTA, version->valuestring);
                strcat(verOTA, ".BIN");

                printf("CONSOLE: Update file=%s\n", verOTA);

                xEventGroupSetBits(otaEventGroup, OTA_WAIT_BIT);
            }
        }

        const cJSON *hubName = cJSON_GetObjectItem(parsed, "IoTHubHostName");
        if(hubName)
        {
            if(cJSON_IsString(hubName))
            {
                printf("CONSOLE: IoTHubHostName=%s\n", hubName->valuestring);
                if(strcmp(hubName->valuestring, IoTHubHandle.hostName) != 0)
                {
                    if(Nvs_Write_String(STORAGE_KEY_IOT_HUB_HOST_NAME, hubName->valuestring) == false)
                    {
                        printf("NVS: Save IoTHubHostName fail\n");
                    }
                    else
                    {
                        printf("NVS: IoTHubHostName=%s\n", hubName->valuestring);
                    }
                }
                else
                {
                    printf("NVS: No change\n");
                }
            }
        }

        const cJSON *deviceId = cJSON_GetObjectItem(parsed, "DeviceId");
        if(deviceId)
        {
            if(cJSON_IsString(deviceId))
            {
                printf("CONSOLE: DeviceId=%s\n", deviceId->valuestring);
                if(strcmp(deviceId->valuestring, IoTHubHandle.deviceId) != 0)
                {
                    if(Nvs_Write_String(STORAGE_KEY_IOT_HUB_DEVICE_ID, deviceId->valuestring) == false)
                    {
                        printf("NVS: Save DeviceId fail\n");
                    }
                    else
                    {
                        printf("NVS: DeviceId=%s\n", deviceId->valuestring);
                    }
                }
                else
                {
                    printf("NVS: No change\n");
                }
            }
        }

        const cJSON *symmetricKey = cJSON_GetObjectItem(parsed, "SymmetricKey");
        if(symmetricKey)
        {
            if(cJSON_IsString(symmetricKey))
            {
                printf("CONSOLE: SymmetricKey=%s\n", symmetricKey->valuestring);
                if(strcmp(symmetricKey->valuestring, IoTHubHandle.symmetricKey) != 0)
                {
                    if(Nvs_Write_String(STORAGE_KEY_IOT_HUB_SYMMETRIC_KEY, symmetricKey->valuestring) == false)
                    {
                        printf("NVS: Save SymmetricKey fail\n");
                    }
                    else
                    {
                        printf("NVS: SymmetricKey=%s\n", symmetricKey->valuestring);
                    }
                }
                else
                {
                    printf("NVS: No change\n");
                }
            }
        }


        cJSON_Delete(parsed);

    }
    else
    {
        printf("CONSOLE: Data must json format\n");
    }
}

void User_Console_Task(void)
{
    uart_init();

    // xTaskCreate(uart_event_task, "uart_event_task", 4096, NULL, 12, NULL);

    while(1)
    {
        if (xQueueReceive(uartQueue, &event, portMAX_DELAY)) 
        {
            switch (event.type) 
            {
                case UART_PATTERN_DET:

                    int pos = uart_pattern_pop_pos(UART_NUM_0);
                    if (pos != -1) 
                    {
                        // Read up to the newline character (including it)
                        int len = uart_read_bytes(UART_NUM_0, consoleHandle.rxBuf, (pos + 1 < CONSOLE_RX_BUF_LEN) ? pos + 1 : CONSOLE_RX_BUF_LEN - 1, 200 / portTICK_PERIOD_MS);
                        if (len > 0) 
                        {
                            // Remove the newline character
                            if (consoleHandle.rxBuf[len - 1] == '\n')
                            {
                                consoleHandle.rxBuf[len - 1] = '\0';
                                len--;
                            }
                            printf("CONSOLE: RX Packet %s, %d bytes\n", consoleHandle.rxBuf, len);
                            consoleHandle.rxLen = len;
                            consoleHandle.isReceived = true;
                        }
                    }
                    break;

                case UART_DATA:
                    // Ignore intermediate UART_DATA events - wait for UART_PATTERN_DET
                    // Data is buffered in the UART driver until pattern is detected
                    break;

                case UART_FIFO_OVF:
                    printf("CONSOLE: RX FIFO overflow\n");
                    uart_flush_input(UART_NUM_0);
                    xQueueReset(uartQueue);
                    break;

                case UART_BUFFER_FULL:
                    printf("CONSOLE: RX buffer full\n");
                    uart_flush_input(UART_NUM_0);
                    xQueueReset(uartQueue);
                    break;

                default:
                    printf("CONSOLE: Unexpected UART event type: %d\n", event.type);
                    break;
            }
        }

        if(consoleHandle.isReceived)
        {
            consoleHandle.isReceived = false;

            User_Console_Handle(consoleHandle.rxBuf);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}










