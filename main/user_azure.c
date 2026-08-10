#include "user_azure.h"
#include "user_ota.h"
#include "user_external_flash.h"
#include "esp_ota_ops.h"
#include "azure_sample_connection.h"
#include "esp_heap_caps.h"

/* Azure Provisioning/IoT Hub library includes */
#include "azure_iot_hub_client.h"
#include "azure_iot_hub_client_properties.h"
#include "azure_iot_provisioning_client.h"

/* Azure JSON includes */
#include "azure_iot_json_reader.h"
#include "azure_iot_json_writer.h"

/* Exponential backoff retry include. */
#include "backoff_algorithm.h"

/* Transport interface implementation include header for TLS. */
#include "transport_tls_socket.h"
#include "transport_socket.h"

/* Crypto helper header. */
#include "azure_sample_crypto.h"

/* Demo Specific configs. */
#include "demo_config.h"

/* Demo Specific Interface Functions. */
#include "azure_sample_connection.h"

bool bIsOtaActivated = false;

/* Data Interface Definition */
// #include "sample_azure_iot_pnp_data_if.h"
#include "wifi_config_manager.h"
#include "user_system.h"
#include "user_azure.h"
#include "user_system.h"
#include "user_ouput.h"
#include "user_ota.h"
#include "user_fram.h"
#include "esp_timer.h"
#include "freertos/event_groups.h"

#include "cJSON.h"
#include "queue.h"
#include "FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include <user_fram.h>
#include "RS485.h"  /* Thu vien dieu khien bien tan INVT GD200A qua Modbus RTU */

#undef AZLogInfo
#define AZLogInfo(...)

/* Declare task handle */
TaskHandle_t Azure_Process_Handle;
TaskHandle_t Azure_Transmit_Handle;

/* Declare IoT hub handle */
IoTHubHandle_t IoTHubHandle;

/* Declare mutex for receive and transmit proceess */
SemaphoreHandle_t azureMutex;

/* Declare queue for command and telemetry data */
QueueHandle_t xQueueTelemetry;
QueueHandle_t xQueueResponse;

/* Task Function Prototype */
static void Azure_Process_Loop_Task(void *pvParameters);
static void Azure_Transmit_Task(void *pvParameters);

/* Ensure NetworkContext is a complete type for stack allocation. The
 * middleware headers typedef an opaque struct; provide a minimal
 * definition used by the sample demos. */
struct NetworkContext
{
    void * pParams;
};

/* Some demo symbols are provided by the sample application. Provide
 * minimal fallbacks here when they are not supplied by the project so
 * the file builds. These are guarded so a real sample implementation
 * will take precedence if present. */
#ifndef democonfigNETWORK_BUFFER_SIZE
#define democonfigNETWORK_BUFFER_SIZE    1024U
#endif

AzureIoTHubClient_t xAzureIoTHubClient;

static uint8_t ucMQTTMessageBuffer[ democonfigNETWORK_BUFFER_SIZE ];

uint32_t ulScratchBufferLength = 0U;
NetworkCredentials_t xNetworkCredentials = { 0 };
AzureIoTTransportInterface_t xTransport;
NetworkContext_t xNetworkContext = { 0 };
TlsTransportParams_t xTlsTransportParams = { 0 };
AzureIoTResult_t xResult;
uint32_t ulStatus;
AzureIoTHubClientOptions_t xHubOptions = { 0 };
bool xSessionPresent;

uint8_t * pucIotHubHostname = (uint8_t *)IoTHubHandle.hostName;
uint8_t * pucIotHubDeviceId = (uint8_t *)IoTHubHandle.deviceId;

/* Simple unix time provider fallback. If your project provides a
 * higher-precision implementation, it will be used instead. */
uint64_t ullGetUnixTime( void )
{
    time_t now = time( NULL );

    /* If system time is set (non-zero and reasonable), return epoch seconds.
     * Otherwise fall back to tick-based uptime (best-effort). */
    if( now > ( time_t ) 1600000000 )
    {
        return ( uint64_t ) now;
    }

    return ( uint64_t )( xTaskGetTickCount() * portTICK_PERIOD_MS / 1000ULL );
}

/* Minimal stubs for platform/sample helper functions. Real
 * implementations should be provided by the project (these return
 * success so demos can compile). */
static uint32_t prvSetupNetworkCredentials( NetworkCredentials_t * pxNetworkCredentials )
{
    pxNetworkCredentials->xDisableSni = pdFALSE;

    /* Set the credentials for establishing a TLS connection. */
    pxNetworkCredentials->pucRootCa = ( const unsigned char * ) democonfigROOT_CA_PEM;
    pxNetworkCredentials->xRootCaSize = sizeof( democonfigROOT_CA_PEM );

#ifdef democonfigCLIENT_CERTIFICATE_PEM
    pxNetworkCredentials->pucClientCert = ( const unsigned char * ) democonfigCLIENT_CERTIFICATE_PEM;
    pxNetworkCredentials->xClientCertSize = sizeof( democonfigCLIENT_CERTIFICATE_PEM );
    pxNetworkCredentials->pucPrivateKey = ( const unsigned char * ) democonfigCLIENT_PRIVATE_KEY_PEM;
    pxNetworkCredentials->xPrivateKeySize = sizeof( democonfigCLIENT_PRIVATE_KEY_PEM );
#endif

    return 0U;
}
static void prvFormatScheduleTime(time_t ts, char *buf, size_t buf_len);
static uint32_t prvConnectToServerWithBackoffRetries( const char * pcHostName, uint32_t ulPort, NetworkCredentials_t * pxNetworkCredentials, NetworkContext_t * pxNetworkContext )
{
    if( ( pcHostName == NULL ) || ( pxNetworkCredentials == NULL ) || ( pxNetworkContext == NULL ) )
    {
        return 1U;
    }

    /* Use the TLS socket connect implementation to initialize transport
     * internals (this will allocate and set pxTlsParams->xSSLContext). */
    TlsTransportStatus_t xTlsStatus = TLS_Socket_Connect( pxNetworkContext,
                                                          pcHostName,
                                                          ( uint16_t ) ulPort,
                                                          pxNetworkCredentials,
                                                          8000U,   /* ulReceiveTimeoutMs – tăng lên 8s để Socket không block vô hạn khi mạng chập chờn */
                                                          3000U ); /* ulSendTimeoutMs - giảm từ 8s xuống 3s để thoát block sớm nếu nghẽn mạng */

    return ( xTlsStatus == eTLSTransportSuccess ) ? 0U : 1U;
}

static void prvHandleCloudMessageTest( AzureIoTHubClientCloudToDeviceMessageRequest_t * pxMessage,
                                      void * pvContext )
{
    ESP_LOGI("AZURE:", "Receive C2D message: %.*s", (int)pxMessage->ulPayloadLength, (const char *)pxMessage->pvMessagePayload);
}

static void prvHandleCommand( AzureIoTHubClientCommandRequest_t * pxMessage, void * pvContext )
{
    uint16_t _code = 99;

    cJSON *res = cJSON_CreateObject();
    DirectMethodResponse_t response;
    memset(&response, 0, sizeof(response));

    ESP_LOGI("AZURE","\n\n");
    ESP_LOGW("AZURE: ", "---------- Direct method ----------");
    ESP_LOGI("AZ: Direct method", "%.*s\nCommand name: %.*s", (int)pxMessage->ulPayloadLength, (char *)pxMessage->pvMessagePayload, (int)pxMessage->usCommandNameLength, (char *)pxMessage->pucCommandName);
    
    if(strncmp((const char *)pxMessage->pucCommandName, "Control", strlen("Control")) == 0 || strncmp((const char *)pxMessage->pucCommandName, "Update", strlen("Update")) == 0 || strncmp((const char *)pxMessage->pucCommandName, "Check", strlen("Check")) == 0)
    {
        ESP_LOGI("AZURE: ", "Received direct method");

        char *buf = calloc(pxMessage->ulPayloadLength + 1, sizeof(char));
        if(buf != NULL)
        {
            memcpy(buf, pxMessage->pvMessagePayload, pxMessage->ulPayloadLength);
            buf[pxMessage->ulPayloadLength] = '\0';

            User_External_Flash_Log_Azure_Command(buf);

            cJSON *parsed = cJSON_Parse(buf);
            cJSON *code = cJSON_GetObjectItem(parsed, "Code");
            if(code)
            {
                _code = code->valueint;
            }

            if(parsed)
            {
                Azure_Handle_Direct_Method_Data(parsed, &response);
                cJSON_Delete(parsed);
            }
            else ESP_LOGE("AZURE: ", "Parse json fail");
            free(buf);
        }
        else
        {
            ESP_LOGE("AZURE: CMD CALLBACK", "Cannot allocate buffer for message");
            response.status = COMMAND_STATUS_DEVICE_ERROR;
            response.payloadLength = sprintf(response.payload, "Payload too long, device not enough ram");
        }
    }
    else
    {
        ESP_LOGE("AZURE: CMD CALLBACK", "Do not have Control method");
        response.status = COMMAND_STATUS_NOT_FOUND;
        response.payloadLength = sprintf(response.payload, "Method name: %.*s do not support", pxMessage->usCommandNameLength, pxMessage->pucCommandName);
    }

    cJSON_AddNumberToObject(res, "Code", _code);
    if(_code == CMD_CODE_ASK_VERSION)
    {
        char formatted_time[32];
        prvFormatScheduleTime(Sys_Info.epochtime, formatted_time, sizeof(formatted_time));
        cJSON_AddStringToObject(res, "Time", formatted_time);
    }
    else cJSON_AddNumberToObject(res, "TimeStamp", Sys_Info.epochtime);
    
    cJSON_AddStringToObject(res, "Message", response.payload);
    char *jsonStr = cJSON_PrintUnformatted(res);

    AzureIoTHubClient_SendCommandResponse(&xAzureIoTHubClient, pxMessage, response.status, (const uint8_t *)jsonStr, strlen(jsonStr));

    if(response.status == COMMAND_STATUS_OK)
    {
        PushTelemetry(jsonStr);
        ESP_LOGI("AZURE", "Pushed telemetry for CMD_CODE %d", _code);
    }

    cJSON_Delete(res);
    free(jsonStr);

    ESP_LOGI(" ","\n\n");
}

static int prvGetDeviceIndexById(uint16_t deviceId)
{
    for(int i = 0; i < DEVICE_MAX_NUM; i++)
    {
        if(DeviceHandle.Device[i].id == deviceId)
        {
            if(!DeviceHandle.Device[i].isActived)
            {
                ESP_LOGW("AZURE", "Device ID %d is DISABLED in current Pond Mode (%d)",
                         deviceId, Sys_Info.pondMode);
                return -1;
            }
            return i;
        }
    }

    return -1;
}

static void prvScheduleApplyToDevice(Device_Parameters_t *dev, const Device_Schedule_t *sch)
{
    dev->isScheduled = true;
    dev->startTime = sch->startTime;
    dev->stopTime = sch->stopTime;
    if(dev->stopTime > dev->startTime)
    {
        dev->duration = (uint32_t)(dev->stopTime - dev->startTime);
    }
    else dev->duration = 0; 
}

static bool prvDevice_AddSchedule(Device_Parameters_t *dev,
                                  const Device_Schedule_t *sch,
                                  bool *applied_now,
                                  char *err_buf,
                                  size_t err_buf_len)
{
    uint8_t new_index;

    if(applied_now != NULL)
    {
        *applied_now = false;
    }

    if(dev->scheduleCount >= DEVICE_SCHEDULE_MAX)
    {
        if(err_buf != NULL)
        {
            snprintf(err_buf, err_buf_len, "Schedule list full");
        }
        return false;
    }

    if(dev->scheduleCount == 0)
    {
        dev->schedules[0] = *sch;
        dev->scheduleCount = 1;
        dev->scheduleIndex = 0;
        if(applied_now != NULL)
        {
            *applied_now = true;
        }
        return true;
    }

    uint8_t last_idx = (uint8_t)(dev->scheduleCount - 1);
    Device_Schedule_t *last = &dev->schedules[last_idx];

    if(!(sch->startTime > last->startTime && sch->startTime > last->stopTime))
    {
        if(err_buf != NULL)
        {
            snprintf(err_buf, err_buf_len, "Invalid Timeline Schedule");
        }
        return false;
    }

    new_index = dev->scheduleCount;
    dev->schedules[new_index] = *sch;
    dev->scheduleCount++;

    if(!dev->isScheduled)
    {
        dev->scheduleIndex = new_index;
        if(applied_now != NULL)
        {
            *applied_now = true;
        }
    }

    return true;
}

static bool prvDevice_ModifySchedule(Device_Parameters_t *dev,
                                     uint8_t schedule_index,
                                     const Device_Schedule_t *sch,
                                     bool *applied_now,
                                     char *err_buf,
                                     size_t err_buf_len)
{
    if(applied_now != NULL)
    {
        *applied_now = false;
    }

    if(dev->scheduleCount == 0 || schedule_index >= dev->scheduleCount)
    {
        if(err_buf != NULL)
        {
            snprintf(err_buf, err_buf_len, "Invalid Schedule Index");
        }
        return false;
    }

    if(schedule_index > 0)
    {
        Device_Schedule_t *prev = &dev->schedules[schedule_index - 1];
        if(!(sch->startTime > prev->stopTime))
        {
            if(err_buf != NULL)
            {
                snprintf(err_buf, err_buf_len, "Invalid Schedule Modify");
            }
            return false;
        }
    }

    if((schedule_index + 1) < dev->scheduleCount)
    {
        Device_Schedule_t *next = &dev->schedules[schedule_index + 1];
        if(!(sch->stopTime < next->startTime))
        {
            if(err_buf != NULL)
            {
                snprintf(err_buf, err_buf_len, "Invalid Schedule Modify");
            }
            return false;
        }
    }

    dev->schedules[schedule_index] = *sch;
    dev->schedules[schedule_index].isFinished = 0;

    if(applied_now != NULL && schedule_index == dev->scheduleIndex)
    {
        *applied_now = true;
    }

    return true;
}

static void prvFormatScheduleTime(time_t ts, char *buf, size_t buf_len)
{
    struct tm tm_time = {0};

    if(buf == NULL || buf_len == 0)
    {
        return;
    }

    if(ts <= 0 || localtime_r(&ts, &tm_time) == NULL)
    {
        snprintf(buf, buf_len, "N/A");
        return;
    }

    strftime(buf, buf_len, "%d/%m/%Y %H:%M:%S", &tm_time);
}

static void prvClearAppliedSchedule(Device_Parameters_t *dev)
{
    dev->isScheduled = false;
    dev->startTime = 0;
    dev->stopTime = 0;
    dev->duration = 0;
    dev->scheduleIndex = 0;
}

static bool prvDevice_DeleteSchedule(Device_Parameters_t *dev,
                                     uint8_t schedule_index,
                                     bool *deleted_current,
                                     Device_Schedule_t *deleted_schedule,
                                     char *err_buf,
                                     size_t err_buf_len)
{
    uint8_t old_current_index;

    if(deleted_current != NULL)
    {
        *deleted_current = false;
    }

    if(dev->scheduleCount == 0 || schedule_index >= dev->scheduleCount)
    {
        if(err_buf != NULL)
        {
            snprintf(err_buf, err_buf_len, "Invalid Schedule Index");
        }
        return false;
    }

    old_current_index = dev->scheduleIndex;
    if(deleted_schedule != NULL)
    {
        *deleted_schedule = dev->schedules[schedule_index];
    }

    if(deleted_current != NULL && schedule_index == old_current_index)
    {
        *deleted_current = true;
    }

    for(uint8_t i = schedule_index; (i + 1U) < dev->scheduleCount; i++)
    {
        dev->schedules[i] = dev->schedules[i + 1U];
    }

    memset(&dev->schedules[dev->scheduleCount - 1U], 0, sizeof(dev->schedules[0]));
    dev->scheduleCount--;

    if(dev->scheduleCount == 0)
    {
        prvClearAppliedSchedule(dev);
        return true;
    }

    if(schedule_index < old_current_index)
    {
        dev->scheduleIndex = (uint8_t)(old_current_index - 1U);
    }
    else if(schedule_index == old_current_index)
    {
        if(old_current_index >= dev->scheduleCount)
        {
            dev->scheduleIndex = (uint8_t)(dev->scheduleCount - 1U);
        }
        else dev->scheduleIndex = old_current_index;
        
    }
    else if(dev->scheduleIndex >= dev->scheduleCount)
    {
        dev->scheduleIndex = (uint8_t)(dev->scheduleCount - 1U);
    }

    prvScheduleApplyToDevice(dev, &dev->schedules[dev->scheduleIndex]);
    return true;
}


/* Provide the sample connection check used by the Azure demos. Forward
 * to the project's network status function so existing system code is
 * reused. */
bool xAzureSample_IsConnectedToInternet( void )
{
    return Is_System_Internet_Connected();
}

void User_Azure_Connect(void)
{
    uint32_t pulIothubHostnameLength = strlen(IoTHubHandle.hostName);
    uint32_t pulIothubDeviceIdLength = strlen(IoTHubHandle.deviceId);

    /* Initialize Azure IoT Middleware - ignore if already initialized */
    AzureIoTResult_t initResult = AzureIoT_Init();
    if(initResult != eAzureIoTSuccess)
    {
        ESP_LOGW("AZURE", "AzureIoT_Init returned %d (may already be initialized)", initResult);
    }

    ulStatus = prvSetupNetworkCredentials( &xNetworkCredentials );
    if( ulStatus != 0 )
    {
        ESP_LOGE("AZURE", "prvSetupNetworkCredentials FAILED (%lu)", ulStatus);
    }

    xNetworkContext.pParams = &xTlsTransportParams;

    /* Retry vô hạn cho đến khi kết nối Azure thành công */
    while(1)
    {
        /* Nếu có config mới được lưu từ web portal → thoát để User_Azure_Task
         * reinit với credentials mới. Không return ở đây mà dùng flag để break. */
        if( IoTHubHandle.isNeedReinit )
        {
            ESP_LOGW("AZURE", "New config detected, exiting connect loop to reinit...");
            TLS_Socket_Disconnect(&xNetworkContext);
            return;
        }

        /* Khi switch 2 ON (Config Mode) → không kết nối Azure để tránh chiếm tài nguyên */
        if( User_Input_Check_Is_Active(IO_CONFIG_INPUT_2) == true )
        {
            ESP_LOGW("AZURE", "Config Mode active (Switch 2 ON). Azure connect suspended.");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        if( !xAzureSample_IsConnectedToInternet() )
        {
            ESP_LOGW("AZURE", "No internet, waiting for WiFi before Azure connect...");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        /* Reset CA store / credentials trước mỗi lần thử kết nối
         * (TLS_Socket_Connect tự free CA store khi thất bại, cần setup lại) */
        ulStatus = prvSetupNetworkCredentials( &xNetworkCredentials );
        if( ulStatus != 0 )
        {
            ESP_LOGE("AZURE", "prvSetupNetworkCredentials FAILED (%lu). Retrying in 10s...", ulStatus);
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        ulStatus = prvConnectToServerWithBackoffRetries( ( const char * ) pucIotHubHostname, democonfigIOTHUB_PORT, &xNetworkCredentials, &xNetworkContext );
        if( ulStatus != 0 )
        {
            ESP_LOGE("AZURE", "TLS connect to Azure Hub FAILED. Retrying in 10s...");
            /* Giải phóng socket và bộ nhớ mbedTLS để tránh rò rỉ tài nguyên (resource leak).
             * Nếu không gọi hàm này, mỗi lần thử kết nối thất bại sẽ tích lũy socket fd và
             * heap mbedTLS không được giải phóng, khiến ESP32 cạn kiệt bộ nhớ sau vài lần thử
             * và bị kẹt mãi trong vòng lặp này dù mạng đã hoạt động bình thường trở lại. */
            TLS_Socket_Disconnect(&xNetworkContext);
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        /* Fill in Transport Interface send and receive function pointers. */
        xTransport.pxNetworkContext = &xNetworkContext;
        xTransport.xSend = TLS_Socket_Send;
        xTransport.xRecv = TLS_Socket_Recv;

        /* Init IoT Hub option */
        xResult = AzureIoTHubClient_OptionsInit( &xHubOptions );
        if( xResult != eAzureIoTSuccess )
        {
            ESP_LOGE("AZURE", "AzureIoTHubClient_OptionsInit FAILED. Retrying in 10s...");
            TLS_Socket_Disconnect(&xNetworkContext);
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        xHubOptions.pucModuleID = ( const uint8_t * ) democonfigMODULE_ID;
        xHubOptions.ulModuleIDLength = sizeof( democonfigMODULE_ID ) - 1;
        xHubOptions.pucModelID = ( const uint8_t * ) sampleazureiotMODEL_ID;
        xHubOptions.ulModelIDLength = sizeof( sampleazureiotMODEL_ID ) - 1;

        xResult = AzureIoTHubClient_Init( &xAzureIoTHubClient,
                                            pucIotHubHostname, pulIothubHostnameLength,
                                            pucIotHubDeviceId, pulIothubDeviceIdLength,
                                            &xHubOptions,
                                            ucMQTTMessageBuffer, sizeof( ucMQTTMessageBuffer ),
                                            ullGetUnixTime,
                                            &xTransport );
        if( xResult != eAzureIoTSuccess )
        {
            ESP_LOGE("AZURE", "AzureIoTHubClient_Init FAILED. Retrying in 10s...");
            TLS_Socket_Disconnect(&xNetworkContext);
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        #ifdef democonfigDEVICE_SYMMETRIC_KEY
            xResult = AzureIoTHubClient_SetSymmetricKey( &xAzureIoTHubClient,
                                                            ( const uint8_t * )IoTHubHandle.symmetricKey,
                                                            strlen((const char *)IoTHubHandle.symmetricKey),
                                                            Crypto_HMAC );
            if( xResult != eAzureIoTSuccess )
            {
                ESP_LOGE("AZURE", "AzureIoTHubClient_SetSymmetricKey FAILED. Retrying in 10s...");
                TLS_Socket_Disconnect(&xNetworkContext);
                vTaskDelay(pdMS_TO_TICKS(10000));
                continue;
            }
        #endif /* democonfigDEVICE_SYMMETRIC_KEY */

        LogInfo( ( "Creating an MQTT connection to %s.\r\n", pucIotHubHostname ) );

        xResult = AzureIoTHubClient_Connect( &xAzureIoTHubClient, false, &xSessionPresent, 5000U );
        if( xResult != eAzureIoTSuccess )
        {
            ESP_LOGE("AZURE", "AzureIoTHubClient_Connect FAILED. Retrying in 10s...");
            TLS_Socket_Disconnect(&xNetworkContext);
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        xResult = AzureIoTHubClient_SubscribeCloudToDeviceMessage( &xAzureIoTHubClient, prvHandleCloudMessageTest, &xAzureIoTHubClient, 5000U );
        if( xResult != eAzureIoTSuccess )
        {
            ESP_LOGE("AZURE", "SubscribeCloudToDeviceMessage FAILED. Retrying in 10s...");
            TLS_Socket_Disconnect(&xNetworkContext);
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        xResult = AzureIoTHubClient_SubscribeCommand( &xAzureIoTHubClient, prvHandleCommand, &xAzureIoTHubClient, 5000U );
        if( xResult != eAzureIoTSuccess )
        {
            ESP_LOGE("AZURE", "SubscribeCommand FAILED. Retrying in 10s...");
            TLS_Socket_Disconnect(&xNetworkContext);
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        /* Kết nối thành công! */
        IoTHubHandle.isAzureInitialized = true;
        ESP_LOGI("AZURE", "Connected to Azure IoT Hub successfully!");

        char az_detail[128];
        snprintf(az_detail, sizeof(az_detail), "Host: %s", IoTHubHandle.hostName);
        User_External_Flash_Log_Event("AZURE_CONNECTED", az_detail);

        // Báo cho OTA Bootloader biết FW mới kết nối mượt mà, huỷ bỏ Rollback
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI("AZURE", "App marked as valid, OTA rollback cancelled.");

        if(IoTHubHandle.isProcessLoopInitialized == false)
        {
            if(xTaskCreatePinnedToCore(Azure_Process_Loop_Task, "Azure process loop", 2*4096, NULL, 5, &Azure_Process_Handle, 0) == pdPASS)
            {
                ESP_LOGI("AZURE: PROCESS LOOP", "Create process loop task suscessfully");
            }
            else ESP_LOGI("AZURE: PROCESS LOOP", "Create process loop task fail\n");
        }

        if(IoTHubHandle.isTransmitInitialized == false)
        {
            if(xTaskCreatePinnedToCore(Azure_Transmit_Task, "Azure transmit", 2*4096, NULL, 5, &Azure_Transmit_Handle, 0) == pdPASS)
            {
                ESP_LOGI("AZURE: TRANSMIT", "Create transmit task suscessfully");
            }
            else ESP_LOGI("AZURE: TRANSMIT", "Create transmit task fail\n");           
        }

        /* Thoát vòng lặp retry sau khi thành công */
        break;
    }
}

uint16_t User_FRAM_Get_Device_Address(uint8_t index)
{
    return index << 9;   // index * 512
}

static bool prvGetFramSlotByDeviceIndex(uint8_t index, uint8_t *slot_out)
{
    if(slot_out == NULL) return false;   
    switch(index)
    {
        case 0: // PaddleWheel_1
        case 1: // PaddleWheel_2
        case 2: // PaddleWheel_3
        case 3: // PaddleWheel_4
            *slot_out = index;
            return true;
        case 6: // Feeder_1_M1
            *slot_out = 4;
            return true;
        case 7: // Feeder_1_M2
            *slot_out = 5;
            return true;
        case 8: // Syphon_1
            *slot_out = 6;
            return true;
        case 9: // Syphon_2
            *slot_out = 7;
            return true;
        default:
            return false; // AirBlow or unknown: do not store in FRAM
    }
}

bool FRAM_SaveDevice(uint8_t index)
{
    if(index >= DEVICE_MAX_NUM)
    {
        ESP_LOGE("FRAM_SAVE", "Invalid index %d", index);
        return false;
    }

    uint8_t slot = 0;
    if(!prvGetFramSlotByDeviceIndex(index, &slot))
    {
        ESP_LOGI("FRAM_SAVE", "Skip device %d (no schedule storage)", index);
        return false;
    }

    uint16_t addr = User_FRAM_Get_Device_Address(slot);

    Device_Persist_t persist;
    memset(&persist, 0, sizeof(persist));
    persist.id = DeviceHandle.Device[index].id;
    persist.index = DeviceHandle.Device[index].index;
    persist.isScheduled = DeviceHandle.Device[index].isScheduled ? 1 : 0;
    persist.isSchedulePaused = DeviceHandle.Device[index].isSchedulePaused ? 1 : 0;
    persist.state = DeviceHandle.Device[index].state;
    persist.startTime = DeviceHandle.Device[index].startTime;
    persist.stopTime = DeviceHandle.Device[index].stopTime;
    persist.scheduleCount = DeviceHandle.Device[index].scheduleCount;
    persist.scheduleIndex = DeviceHandle.Device[index].scheduleIndex;
    memcpy(persist.schedules, DeviceHandle.Device[index].schedules, sizeof(persist.schedules));

    Fram_Write_Enable();

    Fram_Write_Data(addr, (uint8_t*)&persist, sizeof(Device_Persist_t));

    ESP_LOGI("FRAM_SAVE", "Device %d saved at addr 0x%04X (persist %u bytes)", index, addr, (unsigned)sizeof(Device_Persist_t));

    return true;
}

void FRAM_LoadDevice(uint8_t index)
{
    if(index >= DEVICE_MAX_NUM)
    {
        ESP_LOGE("FRAM_LOAD", "Invalid index %d", index);
        return;
    }

    uint8_t slot = 0;
    if(!prvGetFramSlotByDeviceIndex(index, &slot))
    {
        ESP_LOGI("FRAM_LOAD", "Skip device %d (no schedule storage)", index);
        return;
    }

    uint16_t addr = User_FRAM_Get_Device_Address(slot);
    uint8_t configured_index = DeviceHandle.Device[index].index;

    Device_Persist_t persist;
    memset(&persist, 0, sizeof(persist));

    if(!Fram_Read_Data(addr, (uint8_t*)&persist, sizeof(Device_Persist_t)))
    {
        memset(&DeviceHandle.Device[index], 0, sizeof(Device_Parameters_t));
        ESP_LOGE("FRAM_LOAD", "Read fail at device index %d, RAM cleared", index);
        return;
    }

    if(persist.id != 0)
    {
        DeviceHandle.Device[index].id = persist.id;
        if(persist.index < DEVICE_MAX_NUM)
        {
            DeviceHandle.Device[index].index = persist.index;
        }
        else DeviceHandle.Device[index].index = configured_index;
    }
    else DeviceHandle.Device[index].index = configured_index;
    
    DeviceHandle.Device[index].isScheduled = (persist.isScheduled != 0);
    DeviceHandle.Device[index].isSchedulePaused = (persist.isSchedulePaused != 0);
    DeviceHandle.Device[index].state = persist.state;
    DeviceHandle.Device[index].startTime = persist.startTime;
    DeviceHandle.Device[index].stopTime = persist.stopTime;
    DeviceHandle.Device[index].scheduleCount = persist.scheduleCount;
    DeviceHandle.Device[index].scheduleIndex = persist.scheduleIndex;
    memcpy(DeviceHandle.Device[index].schedules, persist.schedules, sizeof(persist.schedules));

    // Validate schedules to detect layout mismatches or corrupted FRAM data
    bool is_corrupted = false;
    if(DeviceHandle.Device[index].scheduleCount > DEVICE_SCHEDULE_MAX)
    {
        is_corrupted = true;
    }
    else
    {
        for(uint8_t j = 0; j < DeviceHandle.Device[index].scheduleCount; j++)
        {
            time_t start = DeviceHandle.Device[index].schedules[j].startTime;
            time_t stop = DeviceHandle.Device[index].schedules[j].stopTime;
            if((start != 0 && (start < 1600000000LL || start > 4102444800LL)) || (stop != 0 && (stop < 1600000000LL || stop > 4102444800LL)))
            {
                is_corrupted = true;
                break;
            }
        }
    }

    if(is_corrupted)
    {
        ESP_LOGW("FRAM_LOAD", "Detected corrupted schedules in FRAM for device %d (possibly old struct version). Clearing schedules.", index);
        DeviceHandle.Device[index].scheduleCount = 0;
        DeviceHandle.Device[index].scheduleIndex = 0;
        DeviceHandle.Device[index].isScheduled = false;
        DeviceHandle.Device[index].startTime = 0;
        DeviceHandle.Device[index].stopTime = 0;
        memset(DeviceHandle.Device[index].schedules, 0, sizeof(DeviceHandle.Device[index].schedules));
        
        // Save cleaned state back to FRAM
        FRAM_SaveDevice(index);
    }

    if(DeviceHandle.Device[index].stopTime > DeviceHandle.Device[index].startTime)
    {
        DeviceHandle.Device[index].duration = (uint32_t)(DeviceHandle.Device[index].stopTime - DeviceHandle.Device[index].startTime);
    }
    else DeviceHandle.Device[index].duration = 0;
    

//----------------------Calculate used bytes for logging---------------------------//
    size_t used_bytes = offsetof(Device_Persist_t, schedules);
    if(persist.scheduleCount > DEVICE_SCHEDULE_MAX)
    {
        persist.scheduleCount = DEVICE_SCHEDULE_MAX;
    }
    used_bytes += (size_t)persist.scheduleCount * sizeof(Device_Schedule_t);
    if(used_bytes > sizeof(Device_Persist_t)) used_bytes = sizeof(Device_Persist_t);
    
    ESP_LOGI("FRAM_LOAD", "Device %d loaded from addr 0x%04X (persist %u bytes, using %u/512byte)",
             index,
             addr,
             (unsigned)sizeof(Device_Persist_t),
             (unsigned)used_bytes);
//----------------------Calculate used bytes for logging---------------------------//
}

void User_Azure_LoadConfig(void)
{
    char az_host[IOT_HUB_HOST_NAME_LEN]     = {0};
    char az_dev[IOT_HUB_DEVICE_ID_LEN]      = {0};
    char az_sym[IOT_HUB_SYMMETRIC_KEY_LEN]  = {0};

    if(azure_config_manager_load(az_host, sizeof(az_host),
                                  az_dev,  sizeof(az_dev),
                                  az_sym,  sizeof(az_sym)))
    {
        strncpy(IoTHubHandle.hostName,     az_host, sizeof(IoTHubHandle.hostName)     - 1);
        strncpy(IoTHubHandle.deviceId,     az_dev,  sizeof(IoTHubHandle.deviceId)     - 1);
        strncpy(IoTHubHandle.symmetricKey, az_sym,  sizeof(IoTHubHandle.symmetricKey) - 1);
        ESP_LOGI("AZURE_CONFIG", "RELOAD NEW AZURE CONFIG: Host=%s, Device=%s", az_host, az_dev);
    }
    else
    {
        /* Xoa credentials trong RAM khi FRAM trống/đã bị clear */
        memset(IoTHubHandle.hostName,     0, sizeof(IoTHubHandle.hostName));
        memset(IoTHubHandle.deviceId,     0, sizeof(IoTHubHandle.deviceId));
        memset(IoTHubHandle.symmetricKey, 0, sizeof(IoTHubHandle.symmetricKey));
        ESP_LOGW("AZURE_CONFIG", "FRAM AUZRE INVALID, DELETE CREDENTIALS IN RAM");
    }

    /* Cập nhật lại các con trỏ toàn cục trỏ vào IoTHubHandle */
    pucIotHubHostname = (uint8_t *)IoTHubHandle.hostName;
    pucIotHubDeviceId = (uint8_t *)IoTHubHandle.deviceId;
}

void User_Azure_Task(void)
{
    /* Create mutex */
    azureMutex = xSemaphoreCreateMutex();

    if(azureMutex == NULL)
    {
        ESP_LOGE("AZURE: CREATE MUTEX", "Cannot create mutex\n\n\n");
        while(1)
        {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    while(1)
    {
        if(!Is_System_Internet_Connected())
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if(strlen(IoTHubHandle.hostName) == 0 || strlen(IoTHubHandle.deviceId) == 0 || strlen(IoTHubHandle.symmetricKey) == 0)
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if(IoTHubHandle.isNeedReinit)
        {
            if(xSemaphoreTake(azureMutex, pdMS_TO_TICKS(3000U)) == pdTRUE)
            {
                IoTHubHandle.isNeedReinit = false;
                AzureIoTHubClient_Deinit(&xAzureIoTHubClient);
                /* Đóng TLS socket cũ trước khi kết nối lại */
                TLS_Socket_Disconnect(&xNetworkContext);
                if(IoTHubHandle.isAzureInitialized)
                {
                    IoTHubHandle.isAzureInitialized = false;
                    User_External_Flash_Log_Event("AZURE_DISCONNECTED", "Reinit requested");
                }
                xSemaphoreGive(azureMutex);
            }
            else ESP_LOGE("AZURE: REINIT", "Cannot get mutex to deinit"); 
        }

        if(bIsOtaActivated)
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if(!IoTHubHandle.isAzureInitialized)
        {
            User_Azure_Connect();
        }

        while(1)
        {
            vTaskDelay(pdMS_TO_TICKS(500));
            if(IoTHubHandle.isNeedReinit || !Is_System_Internet_Connected())
            {
                break;
            }
        }
    }
}

static void Azure_Process_Loop_Task(void *pvParameters)
{

    AzureIoTResult_t result;
    IoTHubHandle.isProcessLoopInitialized = true;

    /* Bộ đếm lỗi liên tiếp: nếu ProcessLoop thất bại >=5 lần liên tiếp
     * → khả năng cao TLS socket bị treo ngầm → trigger reset toàn bộ hệ thống */
    static uint8_t s_processloop_fail_count = 0;
    /* Bộ đếm không lấy được mutex: nếu ProcessLoop task bị block lấy mutex >=10 lần
     * → mutex đang bị deadlock → trigger esp_restart() để giải phóng */
    static uint8_t s_processloop_mutex_fail_count = 0;

    while(1)
    {
        if(!Is_System_Internet_Connected() || !IoTHubHandle.isAzureInitialized || IoTHubHandle.isNeedReinit)
        {
            /* Reset bộ đếm khi hệ thống đang ở trạng thái không kết nối (bình thường) */
            s_processloop_fail_count = 0;
            s_processloop_mutex_fail_count = 0;
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        if(xSemaphoreTake(azureMutex, pdMS_TO_TICKS(5000U)) == pdTRUE)
        {
            s_processloop_mutex_fail_count = 0; /* Reset khi lấy mutex thành công */

            result = AzureIoTHubClient_ProcessLoop(&xAzureIoTHubClient, 100U);
            if(result != eAzureIoTSuccess)
            {
                s_processloop_fail_count++;
                ESP_LOGE("AZURE: PROCESS LOOP", "Error code: %d (consecutive fail: %d/5)",
                         result, s_processloop_fail_count);
                IoTHubHandle.isNeedReinit = true;
                if(IoTHubHandle.isAzureInitialized)
                {
                    IoTHubHandle.isAzureInitialized = false;
                    User_External_Flash_Log_Event("AZURE_DISCONNECTED", "MQTT ProcessLoop Error");
                }

                /* ─── Safety Reset: ProcessLoop thất bại 5 lần liên tiếp ───
                 * Đây là dấu hiệu TLS socket bị treo ngầm hoặc MQTT stack bị corrupt.
                 * Thay vì deadlock mãi, reset ESP32 để khởi động lại sạch sẽ. */
                if(s_processloop_fail_count >= 5)
                {
                    ESP_LOGE("AZURE: PROCESS LOOP",
                             "ProcessLoop failed %d times consecutively – Socket likely hung. Restarting ESP32...",
                             s_processloop_fail_count);
                    //User_External_Flash_Log_Event("AZURE_RESTART", "ProcessLoop hung – safety restart");
                    // Bỏ qua ghi log Flash ở đây để tránh bị kẹt khóa Mutex SPIFFS khi reset khẩn cấp
                    // User_External_Flash_Log_Event("AZURE_RESTART", "ProcessLoop hung – safety restart");
                    vTaskDelay(pdMS_TO_TICKS(500));
                    esp_restart();
                }
            }
            else
            {
                /* Reset bộ đếm khi ProcessLoop thành công */
                s_processloop_fail_count = 0;
            }

            xSemaphoreGive(azureMutex);
        }
        else
        {
            /* Không lấy được mutex trong 5 giây – Mutex đang bị giữ bởi task khác (Deadlock?) */
            s_processloop_mutex_fail_count++;
            ESP_LOGW("AZURE: PROCESS LOOP", "Cannot take mutex (timeout 5s), count: %d/10", s_processloop_mutex_fail_count);

            /* ─── Safety Reset: Deadlock Mutex phát hiện ───
             * Nếu ProcessLoop liên tục không lấy được mutex trong 10 lần × 5s ≈ 50 giây,
             * đây là dấu hiệu Deadlock thực sự. Restart để giải phóng toàn bộ. */
            if(s_processloop_mutex_fail_count >= 10)
            {
                ESP_LOGE("AZURE: PROCESS LOOP", "Mutex Deadlock detected! Restarting ESP32...");
                //User_External_Flash_Log_Event("AZURE_RESTART", "Mutex deadlock – safety restart");
                // Bỏ qua ghi log Flash ở đây để tránh bị kẹt khóa Mutex SPIFFS khi reset khẩn cấp
                // User_External_Flash_Log_Event("AZURE_RESTART", "Mutex deadlock – safety restart");
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void Azure_Transmit_Task(void *pvParameters)
{

    TelemetryEvent_t telemetry;
    // ResponseEvent_t response;
    AzureIoTResult_t result;

    /* Bộ đếm lỗi mutex liên tiếp: nếu Transmit Task không lấy được mutex >= 20 lần
     * liên tiếp (3s × 20 ≈ 60s) → phát hiện Deadlock → gọi esp_restart() */
    static uint8_t s_transmit_mutex_fail_count = 0;

    xQueueTelemetry = xQueueCreate(TELEMETRY_QUEUE_LENGTH, sizeof(TelemetryEvent_t));
    if(xQueueTelemetry == NULL)
    {
        
        while(1)
        {
            ESP_LOGE("AZURE: TRANSMIT TASK", "Cannot create telemetry queue, need to check\n\n\n");
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }
    else
    {
        IoTHubHandle.isTransmitInitialized = true;
    }

    while(1)
    {
        if (xQueueReceive(xQueueTelemetry, &telemetry, 0) == pdTRUE)
        {
            if(!Is_System_Internet_Connected() || !IoTHubHandle.isAzureInitialized || IoTHubHandle.isNeedReinit)
            {
                /* Đây là trạng thái bình thường khi mất mạng/reconnecting → reset bộ đếm deadlock */
                s_transmit_mutex_fail_count = 0;
                ESP_LOGW("AZURE: TRANSMIT TASK", "Skip telemetry (not connected)");
                if (telemetry.payload != NULL)
                {
                    free(telemetry.payload);
                }
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }

            if (xSemaphoreTake(azureMutex, pdMS_TO_TICKS(3000U)) == pdTRUE)
            {
                s_transmit_mutex_fail_count = 0; /* Reset khi lấy được mutex */
                result = AzureIoTHubClient_SendTelemetry(&xAzureIoTHubClient, (const uint8_t *)telemetry.payload, strlen(telemetry.payload), NULL, eAzureIoTHubMessageQoS1, NULL);
                if(result != eAzureIoTSuccess)
                {
                    ESP_LOGE("AZURE: TRANSMIT TASK", "Send telemetry failed: %d", result);
                    IoTHubHandle.isNeedReinit = true;
                    IoTHubHandle.isAzureInitialized = false;
                }
                xSemaphoreGive(azureMutex);
                vTaskDelay(pdMS_TO_TICKS(100)); // Delay nhường quyền cho Azure_Process_Loop_Task xử lý PUBACK
            }
            else
            {
                s_transmit_mutex_fail_count++;
                ESP_LOGE("AZURE: TRANSMIT TASK", "Cannot get mutex (count: %d/40)", s_transmit_mutex_fail_count);

                /* ─── Safety Reset: Mutex Deadlock phát hiện ở Transmit Task ───
                 * Nếu Transmit Task không lấy được mutex 40 lần liên tiếp (3s × 40 ≈ 120s),
                 * hệ thống Azure đang bị deadlock hoàn toàn → restart để thoát. */
                if(s_transmit_mutex_fail_count >= 40)
                {
                    ESP_LOGE("AZURE: TRANSMIT TASK",
                             "Mutex Deadlock detected after %d attempts! Restarting ESP32...",
                             s_transmit_mutex_fail_count);
                    //User_External_Flash_Log_Event("AZURE_RESTART", "Transmit mutex deadlock – safety restart");
                    // Bỏ qua ghi log Flash ở đây để tránh bị kẹt khóa Mutex SPIFFS khi reset khẩn cấp
                    // User_External_Flash_Log_Event("AZURE_RESTART", "Transmit mutex deadlock – safety restart");
                    if (telemetry.payload != NULL)
                    {
                        free(telemetry.payload);
                    }
                    vTaskDelay(pdMS_TO_TICKS(500));
                    esp_restart();
                }
            }

            if (telemetry.payload != NULL)
            {
                free(telemetry.payload);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static char *strdup_psram(const char *s)
{
    if (s == NULL) return NULL;
    size_t len = strlen(s) + 1;
    char *d = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (d != NULL)
    {
        memcpy(d, s, len);
    }
    return d;
}

/* Push Telemetry to queue */
BaseType_t PushTelemetry(const char *payload)
{
    if(IoTHubHandle.isTransmitInitialized)
    {
        TelemetryEvent_t event;
        memset(&event, 0, sizeof(event));
        event.payload = strdup_psram(payload);
        if (event.payload == NULL)
        {
            ESP_LOGE("AZURE: PUSH TELEMETRY", "Failed to allocate dynamic payload");
            return pdFAIL;
        }

        if (xQueueSend(xQueueTelemetry, &event, 0) == pdPASS)
        {
            return pdPASS;
        }
        else
        {
            TelemetryEvent_t discarded_event;
            if (xQueueReceive(xQueueTelemetry, &discarded_event, 0) == pdTRUE)
            {
                ESP_LOGW("AZURE:", "Telemetry queue full, dropped oldest message to fit new one");
                if (discarded_event.payload != NULL)
                {
                    free(discarded_event.payload);
                }
            }
            if (xQueueSend(xQueueTelemetry, &event, 0) != pdPASS)
            {
                free(event.payload);
                return pdFAIL;
            }
            return pdPASS;
        }
    }
    else
    {
        ESP_LOGW("AZURE: PUSH TELEMETRY TO QUEU", "Telemetry do not inited yet");
        return pdFAIL;
    }
}

/* Handle direct method */
void Azure_Handle_Direct_Method_Data(cJSON *payload, DirectMethodResponse_t *response)
{
    uint16_t _code = 0;

    cJSON *code = cJSON_GetObjectItem(payload, "Code");
    cJSON *data = cJSON_GetObjectItem(payload, "Data");

    if((code != NULL) && (data != NULL))
    {
        _code = code->valueint;
        if(_code == CMD_CODE_CONTROL_ON_OFF) //code == 101
        {

            cJSON *deviceName = cJSON_GetObjectItem(data, "DeviceName");
            cJSON *deviceId = cJSON_GetObjectItem(data, "DeviceId");
            cJSON *value = cJSON_GetObjectItem(data, "Value");

            if((deviceName != NULL) && (deviceId != NULL) && (value != NULL))
            {
                ESP_LOGI("AZURE: ", "---------- CMD CALLBACK ----------");
                ESP_LOGI("AZURE: CMD CALLBACK", "Device name: %s, device id: %d, value %d\n", deviceName->valuestring, deviceId->valueint, value->valueint);
                response->status = COMMAND_STATUS_OK;
                response->payloadLength = sprintf(response->payload, "DeviceName: %s, DeviceId: %d, Turn %s", deviceName->valuestring, deviceId->valueint, (value->valueint == 0) ? "OFF" : "ON");

                int index = prvGetDeviceIndexById((uint16_t)deviceId->valueint);
                if(index < 0)
                {
                    response->status = COMMAND_STATUS_BAD_REQUEST;
                    sprintf(response->payload, "Invalid DeviceId");
                    ESP_LOGE("AZURE: ", "Invalid DeviceId %d", deviceId->valueint);
                    goto method_done;
                }

                // if(deviceId->valueint == GROUP_2_DEVICE_ID_1 && value->valueint == 0)
                // {
                //     response->status = COMMAND_STATUS_BAD_REQUEST;
                //     response->payloadLength = sprintf(response->payload, "AirBlow_1 is forced ON");
                //     ESP_LOGW("AZURE: ", "Ignore OFF command for AirBlow_1");
                //     goto method_done;
                // }

                if(deviceId->valueint == GROUP_3_DEVICE_ID_1)
                {
                     if (Sys_Info.feederMode == 0) // Chỉ chặn ở chế độ 2 động cơ cũ
                    {
                        response->status = COMMAND_STATUS_BAD_REQUEST;
                        response->payloadLength = sprintf(response->payload, "Ignore command for Feeder_1_M1");
                        ESP_LOGW("AZURE: ", "Ignore command for Feeder_1_M1");
                        goto method_done;
                    }
                }

                if(value->valueint == 0)
                {
                    if(deviceId->valueint == GROUP_3_DEVICE_ID_2 && Sys_Info.feederMode == 0)
                    {
                        /* Khi OFF DeviceId 32: tắt M1 trước, sau 3s tắt M2 */
                        User_Feeder_M2_Manual_Control(false);
                    }
                    else
                    {
                        DeviceHandle.outputBuf &= ~(1UL << index);
                        if(deviceId->valueint == GROUP_3_DEVICE_ID_2) isManualFeeder_M2 = false;
                    }
                }
                else
                {
                    if(deviceId->valueint == GROUP_3_DEVICE_ID_2 && Sys_Info.feederMode == 0)
                    {
                        /* Khi ON DeviceId 32: bật M2 trước, sau 3s bật M1 */
                        User_Feeder_M2_Manual_Control(true);
                    }
                    else
                    {
                        DeviceHandle.outputBuf |= (1UL << index);
                        if(deviceId->valueint == GROUP_3_DEVICE_ID_2) isManualFeeder_M2 = true;
                    }
                }

                DeviceHandle.activeType = DEVICE_ACTIVE_TYPE_TRIGGER;

                /* Lưu trạng thái bật/tắt tay vào FRAM để phục hồi lại sau khi reset/ngắt điện.
                 * Chỉ bỏ qua việc lưu trực tiếp ở đây đối với M2 khi chạy chế độ 2 động cơ cũ 
                 * (vì trạng thái M2 khi đó được quản lý thông qua delay M1-M2). */
                if(Sys_Info.feederMode == 1 || deviceId->valueint != GROUP_3_DEVICE_ID_2)
                {
                    /* Cập nhật trạng thái trong RAM trước khi lưu vào FRAM */
                    DeviceHandle.Device[index].state = (value->valueint != 0) ? DEVICE_STATE_ON : DEVICE_STATE_OFF;
                    FRAM_SaveDevice((uint8_t)index);
                    ESP_LOGI("AZURE: CMD 101", "Manual state saved to FRAM: Device[%d] = %s", index, (value->valueint != 0) ? "ON" : "OFF");
                }
            }
            else
            {
                response->status = COMMAND_STATUS_BAD_REQUEST;
                sprintf(response->payload, "Bad request");
                ESP_LOGE("AZURE: ", "Control on off Missing important field");
            }

        }
        else if(_code == CMD_CODE_CONTROL_SCHEDULE) //code == 102
        {

            cJSON *startTime = cJSON_GetObjectItem(data, "StartTime");
            cJSON *stopTime = cJSON_GetObjectItem(data, "EndTime");
            cJSON *deviceId = cJSON_GetObjectItem(data, "DeviceId");
            cJSON *deviceName = cJSON_GetObjectItem(data, "DeviceName");

            if((startTime != NULL) && (stopTime != NULL) && (deviceId != NULL) && (deviceName != NULL))
            {
                uint64_t _startTime = (uint64_t)startTime->valuedouble;
                uint64_t _stopTime = (uint64_t)stopTime->valuedouble;
                uint16_t _deviceId = deviceId->valueint;

                if(_deviceId == GROUP_3_DEVICE_ID_1)
                {
                    if (Sys_Info.feederMode == 0) // Chỉ bỏ qua ở chế độ 2 động cơ cũ
                    {
                        response->status = COMMAND_STATUS_OK;
                        response->payloadLength = sprintf(response->payload,
                                                          "Ignore schedule for Feeder_1_M1 (DeviceId %d)",
                                                          _deviceId);
                        ESP_LOGW("AZURE: ", "Ignore schedule for Feeder_1_M1 (DeviceId %d)", _deviceId);
                        goto method_done;
                    }
                }

                if ((Sys_Info.oxyMode == 0 && (_deviceId == GROUP_2_DEVICE_ID_1 || _deviceId == GROUP_2_DEVICE_ID_2)) || (Sys_Info.oxyMode == 1 && _deviceId == Sys_Info.activeOxyId))
                {
                    response->status = COMMAND_STATUS_BAD_REQUEST;
                    response->payloadLength = sprintf(response->payload, "Ignore schedule for Oxy machine (DeviceId %d)", _deviceId);
                    ESP_LOGW("AZURE: ", "Ignore schedule for Oxy machine (DeviceId %d)", _deviceId);
                    goto method_done;
                }

                int second = _startTime % 100;
                _startTime /= 100;
                int minute = _startTime % 100;
                _startTime /= 100;
                int hour = _startTime % 100;
                _startTime /= 100;
                int year = 2000 + (_startTime % 100); // Giả định năm 2 bước là 20xx
                _startTime /= 100;
                int month = _startTime % 100;
                _startTime /= 100;
                int day = _startTime % 100;

                int second_end = _stopTime % 100;
                _stopTime /= 100;
                int minute_end = _stopTime % 100;
                _stopTime /= 100;
                int hour_end = _stopTime % 100;
                _stopTime /= 100;
                int year_end = 2000 + (_stopTime % 100); // Giả định năm 2 bước là 20xx
                _stopTime /= 100;
                int month_end = _stopTime % 100;
                _stopTime /= 100;
                int day_end = _stopTime % 100;

                struct tm t;

                t.tm_year = year - 1900; // Năm tính từ 1900
                t.tm_mon = month - 1;    // Tháng từ 0-11
                t.tm_mday = day;
                t.tm_hour = hour;
                t.tm_min = minute;
                t.tm_sec = second;
                t.tm_isdst = -1; 

                time_t epoch = mktime(&t);
                
                struct tm te;

                te.tm_year = year_end - 1900; // Năm tính từ 1900
                te.tm_mon = month_end - 1;    // Tháng từ 0-11
                te.tm_mday = day_end;
                te.tm_hour = hour_end;
                te.tm_min = minute_end;
                te.tm_sec = second_end;
                te.tm_isdst = -1;

                time_t epoch_end = mktime(&te);
                if(epoch >= epoch_end) //CHECK STARTTIME CÓ LỚN HƠN ENDTIME KHÔNG
                {
                    response->status = COMMAND_STATUS_BAD_REQUEST;
                    response->payloadLength = sprintf(response->payload, "Invalid time: StartTime must be earlier than EndTime");
                    ESP_LOGE("AZURE:", "Invalid schedule time: start=%lld end=%lld", (long long)epoch, (long long)epoch_end);
                    goto method_done;
                }

                // ESP_LOGI("AZURE: ", "Start time: %02d/%02d/%04d %02d:%02d:%02d (epoch - %lld)", day, month, year, hour, minute, second, epoch);
                response->status = COMMAND_STATUS_OK;
                response->payloadLength = snprintf(response->payload,
                                                   sizeof(response->payload),
                                                   "DeviceName: %s, DeviceId: %d, StartTime: %02d/%02d/%04d -- %02d:%02d:%02d, EndTime: %02d/%02d/%04d -- %02d:%02d:%02d",
                                                   deviceName->valuestring,
                                                   deviceId->valueint,
                                                   day, month, year, hour, minute, second,
                                                   day_end, month_end, year_end, hour_end, minute_end, second_end);

                int shift = prvGetDeviceIndexById(_deviceId);
                if(shift < 0)
                {
                    response->status = COMMAND_STATUS_BAD_REQUEST;
                    sprintf(response->payload, "Invalid DeviceId");
                    ESP_LOGE("AZURE: ", "Invalid DeviceId %d", deviceId->valueint);
                    goto method_done;
                }

                // DeviceHandle.outputBuf |= (1UL << shift);
                DeviceHandle.activeType = DEVICE_ACTIVE_TYPE_SCHEDULE;

                Device_Schedule_t new_schedule =
                {
                    .startTime = epoch,
                    .stopTime = epoch_end,
                    .isFinished = 0
                };

                bool applied_now = false;
                char schedule_err[64] = {0};
                if(!prvDevice_AddSchedule(&DeviceHandle.Device[shift],
                                          &new_schedule,
                                          &applied_now,
                                          schedule_err,
                                          sizeof(schedule_err)))
                {
                    response->status = COMMAND_STATUS_BAD_REQUEST;
                    response->payloadLength = snprintf(response->payload,
                                                       sizeof(response->payload),
                                                       "%s",
                                                       (schedule_err[0] != '\0') ? schedule_err : "Invalid Timeline Schedule");
                    ESP_LOGE("AZURE: ", "Schedule rejected: %s", response->payload);
                    goto method_done;
                }

                if(applied_now)
                {
                    prvScheduleApplyToDevice(&DeviceHandle.Device[shift], &new_schedule);
                }

                FRAM_SaveDevice(shift);
                ESP_LOGI("AZURE: ", "Saved schedule to FRAM at device index %d", shift);

                ESP_LOGI("AZURE: ", "Control schedule: %s, device index %d, applied_now=%d, count=%u",
                         response->payload,
                         shift,
                         applied_now ? 1 : 0,
                         (unsigned int)DeviceHandle.Device[shift].scheduleCount);
                for(int i = 0; i<DEVICE_MAX_NUM; i++)
                {
                    ESP_LOGI("AZURE: ", "device[%d].isScheduled=%d, state: %d", i, DeviceHandle.Device[i].isScheduled, DeviceHandle.Device[i].state);
                }
            }
            else
            {
                response->status = COMMAND_STATUS_BAD_REQUEST;
                sprintf(response->payload, "Bad request");
                ESP_LOGE("AZURE: ", "Missing important field");
            }

        }

        else if(_code == CMD_CODE_MODIFY_CONTROL_SCHEDULE) //code == 103
        {
            cJSON *startTime = cJSON_GetObjectItem(data, "StartTime");
            cJSON *stopTime = cJSON_GetObjectItem(data, "EndTime");
            cJSON *scheduleIndex = cJSON_GetObjectItem(data, "Schedule_index");
            cJSON *deviceName = cJSON_GetObjectItem(data, "DeviceName");
            cJSON *deviceId = cJSON_GetObjectItem(data, "DeviceId");

            if((startTime != NULL) && (stopTime != NULL) && (scheduleIndex != NULL) && (deviceId != NULL) && (deviceName != NULL))
            {
                uint64_t _startTime = (uint64_t)startTime->valuedouble;
                uint64_t _stopTime = (uint64_t)stopTime->valuedouble;
                uint16_t _deviceId = deviceId->valueint;
                uint8_t _scheduleIndex = (uint8_t)scheduleIndex->valueint;

                if(_deviceId == GROUP_3_DEVICE_ID_1)
                {
                    if (Sys_Info.feederMode == 0) // Chỉ bỏ qua ở chế độ 2 động cơ cũ
                    {
                        response->status = COMMAND_STATUS_OK;
                        response->payloadLength = sprintf(response->payload, "Ignore schedule for Feeder_1_M1 (DeviceId %d)", _deviceId);
                        ESP_LOGW("AZURE: ", "Ignore schedule for Feeder_1_M1 (DeviceId %d)", _deviceId);
                        goto method_done;
                    }
                }

                if ((Sys_Info.oxyMode == 0 && (_deviceId == GROUP_2_DEVICE_ID_1 || _deviceId == GROUP_2_DEVICE_ID_2)) || (Sys_Info.oxyMode == 1 && _deviceId == Sys_Info.activeOxyId))
                {
                    response->status = COMMAND_STATUS_BAD_REQUEST;
                    response->payloadLength = sprintf(response->payload, "Ignore schedule for Oxy machine (DeviceId %d)", _deviceId);
                    ESP_LOGW("AZURE: ", "Ignore schedule for Oxy machine (DeviceId %d)", _deviceId);
                    goto method_done;
                }

                int second = _startTime % 100;
                _startTime /= 100;
                int minute = _startTime % 100;
                _startTime /= 100;
                int hour = _startTime % 100;
                _startTime /= 100;
                int year = 2000 + (_startTime % 100);
                _startTime /= 100;
                int month = _startTime % 100;
                _startTime /= 100;
                int day = _startTime % 100;

                int second_end = _stopTime % 100;
                _stopTime /= 100;
                int minute_end = _stopTime % 100;
                _stopTime /= 100;
                int hour_end = _stopTime % 100;
                _stopTime /= 100;
                int year_end = 2000 + (_stopTime % 100);
                _stopTime /= 100;
                int month_end = _stopTime % 100;
                _stopTime /= 100;
                int day_end = _stopTime % 100;

                struct tm t;
                t.tm_year = year - 1900;
                t.tm_mon = month - 1;
                t.tm_mday = day;
                t.tm_hour = hour;
                t.tm_min = minute;
                t.tm_sec = second;
                t.tm_isdst = -1;

                time_t epoch = mktime(&t);

                struct tm te;
                te.tm_year = year_end - 1900;
                te.tm_mon = month_end - 1;
                te.tm_mday = day_end;
                te.tm_hour = hour_end;
                te.tm_min = minute_end;
                te.tm_sec = second_end;
                te.tm_isdst = -1;

                time_t epoch_end = mktime(&te);
                if(epoch >= epoch_end)
                {
                    response->status = COMMAND_STATUS_BAD_REQUEST;
                    response->payloadLength = sprintf(response->payload, "Invalid time: StartTime must be earlier than EndTime");
                    ESP_LOGE("AZURE:", "Invalid modify time: start=%lld end=%lld", (long long)epoch, (long long)epoch_end);
                    goto method_done;
                }

                response->status = COMMAND_STATUS_OK;
                response->payloadLength = snprintf(response->payload,
                                                   sizeof(response->payload),
                                                   "Modify successfull schedule index %u ---> DeviceName: %s, DeviceId: %d, StartTime: %02d/%02d/%04d -- %02d:%02d:%02d, EndTime: %02d/%02d/%04d -- %02d:%02d:%02d",
                                                   (unsigned int)_scheduleIndex,
                                                   deviceName->valuestring,
                                                   deviceId->valueint,
                                                   day, month, year, hour, minute, second,
                                                   day_end, month_end, year_end, hour_end, minute_end, second_end);

                int shift = prvGetDeviceIndexById(_deviceId);
                if(shift < 0)
                {
                    response->status = COMMAND_STATUS_BAD_REQUEST;
                    sprintf(response->payload, "Invalid DeviceId");
                    ESP_LOGE("AZURE: ", "Invalid DeviceId %d", deviceId->valueint);
                    goto method_done;
                }

                DeviceHandle.activeType = DEVICE_ACTIVE_TYPE_SCHEDULE;

                Device_Schedule_t new_schedule =
                {
                    .startTime = epoch,
                    .stopTime = epoch_end,
                    .isFinished = 0
                };

                bool applied_now = false;
                char schedule_err[64] = {0};
                if(!prvDevice_ModifySchedule(&DeviceHandle.Device[shift],
                                             _scheduleIndex,
                                             &new_schedule,
                                             &applied_now,
                                             schedule_err,
                                             sizeof(schedule_err)))
                {
                    response->status = COMMAND_STATUS_BAD_REQUEST;
                    response->payloadLength = snprintf(response->payload,
                                                       sizeof(response->payload),
                                                       "%s",
                                                       (schedule_err[0] != '\0') ? schedule_err : "Invalid Schedule Modify");
                    ESP_LOGE("AZURE: ", "Schedule modify rejected: %s", response->payload);
                    goto method_done;
                }

                if(applied_now)
                {
                    prvScheduleApplyToDevice(&DeviceHandle.Device[shift], &new_schedule);
                }

                FRAM_SaveDevice(shift);
                ESP_LOGI("AZURE: ", "Modified schedule saved to FRAM at device index %d", shift);
            }
            else
            {
                response->status = COMMAND_STATUS_BAD_REQUEST;
                sprintf(response->payload, "Bad request");
                ESP_LOGE("AZURE: ", "Modify schedule missing important field");
            }
        }

        else if(_code == CMD_CODE_DELETE_SCHEDULE) //code ==104
        {
            cJSON *scheduleIndex = cJSON_GetObjectItem(data, "Schedule_Index");
            cJSON *deviceName = cJSON_GetObjectItem(data, "DeviceName");
            cJSON *deviceId = cJSON_GetObjectItem(data, "DeviceId");

            if((scheduleIndex != NULL) && (deviceName != NULL) && (deviceId != NULL))
            {
                uint16_t _deviceId = (uint16_t)deviceId->valueint;
                uint8_t _scheduleIndex = (uint8_t)scheduleIndex->valueint;
                int shift = prvGetDeviceIndexById(_deviceId);
                Device_Parameters_t *dev = NULL;
                bool deleted_current = false;
                Device_Schedule_t deleted_schedule = {0};
                char schedule_err[64] = {0};
                char start_time_str[32];
                char end_time_str[32];

                if(shift < 0)
                {
                    response->status = COMMAND_STATUS_BAD_REQUEST;
                    response->payloadLength = snprintf(response->payload, sizeof(response->payload), "Invalid DeviceId");
                    ESP_LOGE("AZURE: ", "Invalid DeviceId %d", deviceId->valueint);
                    goto method_done;
                }

                dev = &DeviceHandle.Device[shift];
                if(dev->isScheduled && dev->scheduleCount > 0 && _scheduleIndex == dev->scheduleIndex && Sys_Info.epochtime >= dev->startTime && Sys_Info.epochtime < dev->stopTime)
                {
                    response->status = COMMAND_STATUS_BAD_REQUEST;
                    response->payloadLength = snprintf(response->payload, sizeof(response->payload), "Active schedule cannot be deleted");
                    ESP_LOGW("AZURE: ", "Reject delete active schedule index %u for device %d", (unsigned int)_scheduleIndex, _deviceId);
                    goto method_done;
                }

                if(!prvDevice_DeleteSchedule(dev, _scheduleIndex, &deleted_current, &deleted_schedule, schedule_err, sizeof(schedule_err)))
                {
                    response->status = COMMAND_STATUS_BAD_REQUEST;
                    response->payloadLength = snprintf(response->payload,
                                                       sizeof(response->payload),
                                                       "%s",
                                                       (schedule_err[0] != '\0') ? schedule_err : "Delete schedule failed");
                    ESP_LOGE("AZURE: ", "Delete schedule rejected: %s", response->payload);
                    goto method_done;
                }

                if(deleted_current)
                {
                    User_Output_Set_Value(0, (uint8_t)shift, &DeviceHandle);
                    DeviceHandle.Device[shift].state = DEVICE_STATE_OFF;

                    if(_deviceId == GROUP_3_DEVICE_ID_2 && Sys_Info.feederMode == 0)
                    {
                        int feeder_m1_index = prvGetDeviceIndexById(GROUP_3_DEVICE_ID_1);
                        if(feeder_m1_index >= 0)
                        {
                            User_Output_Set_Value(0, (uint8_t)feeder_m1_index, &DeviceHandle);
                            DeviceHandle.Device[feeder_m1_index].state = DEVICE_STATE_OFF;
                        }
                    }
                }

                DeviceHandle.activeType = DEVICE_ACTIVE_TYPE_SCHEDULE;

                FRAM_SaveDevice((uint8_t)shift);

                prvFormatScheduleTime(deleted_schedule.startTime, start_time_str, sizeof(start_time_str));
                prvFormatScheduleTime(deleted_schedule.stopTime, end_time_str, sizeof(end_time_str));

                response->status = COMMAND_STATUS_OK;
                response->payloadLength = snprintf(response->payload,
                                                   sizeof(response->payload),
                                                   "Deleted successful Schedule index %u ---> DeviceName: %s, DeviceId: %d, StartTime: %s, EndTime: %s",
                                                   (unsigned int)_scheduleIndex,
                                                   deviceName->valuestring,
                                                   deviceId->valueint,
                                                   start_time_str,
                                                   end_time_str);

                ESP_LOGI("AZURE: ", "Deleted schedule index %u for device index %d, remaining=%u", (unsigned int)_scheduleIndex, shift, (unsigned int)dev->scheduleCount);
            }
            else
            {
                response->status = COMMAND_STATUS_BAD_REQUEST;
                response->payloadLength = snprintf(response->payload, sizeof(response->payload), "Bad request");
                ESP_LOGE("AZURE: ", "Delete schedule missing important field");
            }
        }

        else if(_code == CMD_CODE_SAVE_SCHEDULE_AND_EXECUTE) //code == 105
        {
            cJSON *DeviceId = cJSON_GetObjectItem(payload, "DeviceId");
            
            if(DeviceId != NULL)
            {   
                uint16_t _deviceId = DeviceId->valueint;
                int shift = prvGetDeviceIndexById(_deviceId);

                if(shift < 0)
                {
                    response->status = COMMAND_STATUS_BAD_REQUEST;
                    response->payloadLength = snprintf(response->payload, sizeof(response->payload), "Invalid DeviceId");
                    ESP_LOGE("AZURE: ", "Invalid DeviceId %d for Code 105", _deviceId);
                    goto method_done;
                }

                 if (Sys_Info.feederMode == 0 && _deviceId == GROUP_3_DEVICE_ID_1)
                {
                    response->status = COMMAND_STATUS_BAD_REQUEST;
                    response->payloadLength = snprintf(response->payload, sizeof(response->payload), "Ignore schedule for Feeder_1_M1 (DeviceId %d)", _deviceId);
                    ESP_LOGW("AZURE: ", "Ignore schedule for Feeder_1_M1 (DeviceId %d)", _deviceId);
                    goto method_done;
                }
                if ((Sys_Info.oxyMode == 0 && (_deviceId == GROUP_2_DEVICE_ID_1 || _deviceId == GROUP_2_DEVICE_ID_2)) || (Sys_Info.oxyMode == 1 && _deviceId == Sys_Info.activeOxyId))
                {
                    response->status = COMMAND_STATUS_BAD_REQUEST;
                    response->payloadLength = snprintf(response->payload, sizeof(response->payload), "Ignore schedule for Oxy machine (DeviceId %d)", _deviceId);
                    ESP_LOGW("AZURE: ", "Ignore schedule for Oxy machine (DeviceId %d)", _deviceId);
                    goto method_done;
                }

                Device_Parameters_t *dev = &DeviceHandle.Device[shift];
                
                // Clear existing schedules in RAM
                memset(dev->schedules, 0, sizeof(dev->schedules));
                dev->scheduleCount = 0;
                dev->scheduleIndex = 0;
                dev->isScheduled = false;
                
                if(data != NULL && cJSON_IsArray(data))
                {
                    int arr_size = cJSON_GetArraySize(data);
                    int max_schedules = sizeof(dev->schedules) / sizeof(dev->schedules[0]);
                    if(arr_size > max_schedules) arr_size = max_schedules;
                    
                    for(int i = 0; i < arr_size; i++)
                    {
                        cJSON *item = cJSON_GetArrayItem(data, i);
                        if(item)
                        {
                            cJSON *StartTime = cJSON_GetObjectItem(item, "StartTime");
                            cJSON *EndTime = cJSON_GetObjectItem(item, "EndTime");
                            cJSON *IsFinished = cJSON_GetObjectItem(item, "IsFinished");

                            if(StartTime && EndTime && IsFinished)
                            {
                                uint64_t _startTime = (uint64_t)StartTime->valuedouble;
                                uint64_t _stopTime = (uint64_t)EndTime->valuedouble;

                                int second = _startTime % 100;
                                _startTime /= 100;
                                int minute = _startTime % 100;
                                _startTime /= 100;
                                int hour = _startTime % 100;
                                _startTime /= 100;
                                int year = 2000 + (_startTime % 100);
                                _startTime /= 100;
                                int month = _startTime % 100;
                                _startTime /= 100;
                                int day = _startTime % 100;

                                int second_end = _stopTime % 100;
                                _stopTime /= 100;
                                int minute_end = _stopTime % 100;
                                _stopTime /= 100;
                                int hour_end = _stopTime % 100;
                                _stopTime /= 100;
                                int year_end = 2000 + (_stopTime % 100);
                                _stopTime /= 100;
                                int month_end = _stopTime % 100;
                                _stopTime /= 100;
                                int day_end = _stopTime % 100;

                                struct tm t;
                                t.tm_year = year - 1900;
                                t.tm_mon = month - 1;
                                t.tm_mday = day;
                                t.tm_hour = hour;
                                t.tm_min = minute;
                                t.tm_sec = second;
                                t.tm_isdst = -1; 
                                time_t epoch = mktime(&t);
                                
                                struct tm te;
                                te.tm_year = year_end - 1900;
                                te.tm_mon = month_end - 1;
                                te.tm_mday = day_end;
                                te.tm_hour = hour_end;
                                te.tm_min = minute_end;
                                te.tm_sec = second_end;
                                te.tm_isdst = -1;
                                time_t epoch_end = mktime(&te);

                                // Validate times and durations
                                if(epoch >= epoch_end)
                                {
                                    ESP_LOGE("AZURE:", "Invalid schedule parameters array index %d. Skipping.", i);
                                    continue;
                                }
                                
                                dev->schedules[dev->scheduleCount].startTime = epoch;
                                dev->schedules[dev->scheduleCount].stopTime = epoch_end;
                                dev->schedules[dev->scheduleCount].isFinished = IsFinished->valueint;
                                
                                dev->scheduleCount++;
                                dev->isScheduled = true;
                                if (dev->scheduleCount >= max_schedules)
                                {
                                    break;
                                }
                            }
                        }
                    }
                }

                if (dev->scheduleCount > 0)
                {
                    prvScheduleApplyToDevice(dev, &dev->schedules[0]);
                }
                
                DeviceHandle.activeType = DEVICE_ACTIVE_TYPE_SCHEDULE;
                FRAM_SaveDevice(shift);
                ESP_LOGI("AZURE: ", "Saved %u schedules to FRAM at root device index %d from Code 105", dev->scheduleCount, shift);

                response->status = COMMAND_STATUS_OK;
                response->payloadLength = snprintf(response->payload, sizeof(response->payload), "Saved successfully %u schedules for DeviceId %d", dev->scheduleCount, _deviceId);
            }
            else
            {
                response->status = COMMAND_STATUS_BAD_REQUEST;
                response->payloadLength = snprintf(response->payload, sizeof(response->payload), "Failed to save schedules");
                ESP_LOGE("AZURE: ", "Failed to save schedules");
            }
        }

        else if(_code == CMD_CODE_SET_WIFI_ONLINE) //code == 106
        {
            cJSON *ssid = cJSON_GetObjectItem(data, "SSID");
            cJSON *password = cJSON_GetObjectItem(data, "PASSWORD");

            if ((ssid != NULL) && (password != NULL) && cJSON_IsString(ssid) && cJSON_IsString(password))
            {
                bool ok = wifi_config_manager_save(ssid->valuestring, password->valuestring);
                if (ok)
                {
                    response->status = COMMAND_STATUS_OK;
                    response->payloadLength = snprintf(response->payload, sizeof(response->payload),
                                                      "{\"Code\":106,\"TimeStamp\":%lld,\"Message\":\"SAVE WIFI -->%s<-- SUCCESSFULL\"}",
                                                      (long long)Sys_Info.epochtime, ssid->valuestring);
                    Sys_Info.isRebootRequested = true;
                    Sys_Info.rebootTimestamp = xTaskGetTickCount();
                    ESP_LOGI("AZURE", "Set WiFi Online SUCCESS. Auto-rebooting in 5s...");
                }
                else
                {
                    response->status = COMMAND_STATUS_BAD_REQUEST;
                    response->payloadLength = snprintf(response->payload, sizeof(response->payload),
                                                      "{\"Code\":106,\"TimeStamp\":%lld,\"Message\":\"Error: SAVE WIFI -->%s<-- FAIL\"}",
                                                      (long long)Sys_Info.epochtime, ssid->valuestring);
                    ESP_LOGE("AZURE", "Set WiFi Online FAIL");
                }
            }
            else
            {
                response->status = COMMAND_STATUS_BAD_REQUEST;
                response->payloadLength = snprintf(response->payload, sizeof(response->payload),
                                                  "{\"Code\":106,\"TimeStamp\":%lld,\"Message\":\"Error: Missing SSID or PASSWORD\"}",
                                                  (long long)Sys_Info.epochtime);
            }
        }

        else if(_code == CMD_CODE_SET_AZURE_ONLINE) //code == 107
        {
            cJSON *host = cJSON_GetObjectItem(data, "HostName");
            cJSON *deviceID = cJSON_GetObjectItem(data, "DeviceID");
            cJSON *sym = cJSON_GetObjectItem(data, "SymmetricKey");

            if ((host != NULL) && (deviceID != NULL) && (sym != NULL) && cJSON_IsString(host) && cJSON_IsString(deviceID) && cJSON_IsString(sym))
            {
                bool ok = azure_config_manager_save(host->valuestring, deviceID->valuestring, sym->valuestring);
                if (ok)
                {
                    response->status = COMMAND_STATUS_OK;
                    response->payloadLength = snprintf(response->payload, sizeof(response->payload),
                                                      "{\"Code\":107,\"TimeStamp\":%lld,\"Message\":\"SAVE AZURE -->%s<-- SUCCESSFULL\"}",
                                                      (long long)Sys_Info.epochtime, deviceID->valuestring);
                    Sys_Info.isRebootRequested = true;
                    Sys_Info.rebootTimestamp = xTaskGetTickCount();
                    ESP_LOGI("AZURE", "Set Azure Online SUCCESS. Auto-rebooting in 5s...");
                }
                else
                {
                    response->status = COMMAND_STATUS_BAD_REQUEST;
                    response->payloadLength = snprintf(response->payload, sizeof(response->payload),
                                                      "{\"Code\":107,\"TimeStamp\":%lld,\"Message\":\"Error: SAVE AZURE -->%s<-- FAIL\"}",
                                                      (long long)Sys_Info.epochtime, deviceID->valuestring);
                    ESP_LOGE("AZURE", "Set Azure Online FAIL");
                }
            }
            else
            {
                response->status = COMMAND_STATUS_BAD_REQUEST;
                response->payloadLength = snprintf(response->payload, sizeof(response->payload),
                                                  "{\"Code\":107,\"TimeStamp\":%lld,\"Message\":\"Error: Missing Azure config fields\"}",
                                                  (long long)Sys_Info.epochtime);
            }
        }

        else if(_code == CMD_CODE_SET_TIME_OF_DEVICE_REPORT) //code == 108
        {
            cJSON *TimeReport = cJSON_GetObjectItem(data, "TimeReport");
            if(TimeReport != NULL)
            {
                Sys_Info.timeReportDevice = TimeReport->valueint;
                response->status = COMMAND_STATUS_OK;
                response->payloadLength = snprintf(response->payload, sizeof(response->payload),
                                                  "{\"Code\":108,\"TimeStamp\":%lld,\"Message\":\"SAVE TIME REPORT %uS SUCCESSFULL\"}",
                                                  (long long)Sys_Info.epochtime, (unsigned int)Sys_Info.timeReportDevice);
                ESP_LOGI("AZURE", "Set TimeReport SUCCESS: %u seconds", (unsigned int)Sys_Info.timeReportDevice);
            }
            else
            {
                response->status = COMMAND_STATUS_BAD_REQUEST;
                response->payloadLength = snprintf(response->payload, sizeof(response->payload),
                                                  "{\"Code\":108,\"TimeStamp\":%lld,\"Message\":\"Error: SAVE TIME REPORT FAIL\"}",
                                                  (long long)Sys_Info.epochtime);
                ESP_LOGE("AZURE", "Set TimeReport FAIL: Missing TimeReport field");
            }
        }

        else if(_code == CMD_CODE_SET_TIME_OF_VFD_REPORT) //code == 113
        {
            response->status = COMMAND_STATUS_OK;
            response->payloadLength = snprintf(response->payload, sizeof(response->payload),
                                              "{\"Code\":113,\"TimeStamp\":%lld,\"Message\":\"VFD TimeReport is deprecated, unified telemetry uses Device TimeReport (Code 108)\"}",
                                              (long long)Sys_Info.epochtime);
            ESP_LOGW("AZURE", "Set VFD TimeReport (Code 113) is deprecated. Using unified Device TimeReport (Code 108) instead.");
        }

        else if(_code == CMD_CODE_CLEAR_FRAM) //code == 109
        {
            ESP_LOGW("AZURE", "---------- CMD CLEAR ENTIRE FRAM ----------");
            
            // 1. Clear device slots (schedules & persistent params)
            FRAM_Delete_All();

            // 2. Clear saved output buffer and fault states in FRAM (set to 0xFFFFFFFF)
            uint32_t ff = 0xFFFFFFFF;
            Fram_Write_Data(0x1600, (uint8_t *)&ff, sizeof(uint32_t));
            Fram_Write_Data(0x1604, (uint8_t *)&ff, sizeof(uint32_t));

            // 3. Flush all physical outputs to OFF immediately
            User_Out_Put_Flush_All(0);

            // 4. Re-initialize default devices in DeviceHandle RAM
            User_Device_Init();

            response->status = COMMAND_STATUS_OK;
            response->payloadLength = snprintf(response->payload, sizeof(response->payload),
                                              "{\"Code\":109,\"TimeStamp\":%lld,\"Message\":\"CLEAR ENTIRE FRAM SUCCESSFULL. REBOOTING...\"}",
                                              (long long)Sys_Info.epochtime);
            ESP_LOGI("AZURE", "Clear FRAM successfully. Requesting auto-reboot in 5 seconds.");

            Sys_Info.isRebootRequested = true;
            Sys_Info.rebootTimestamp = xTaskGetTickCount();
        }

        else if(_code == CMD_CODE_RESET_ESP) //code == 110
        {
            ESP_LOGW("AZURE", "---------- CMD RESET ESP ----------");
            response->status = COMMAND_STATUS_OK;
            response->payloadLength = snprintf(response->payload, sizeof(response->payload),
                                              "{\"Code\":110,\"TimeStamp\":%lld,\"Message\":\"RESET ESP SUCCESSFULL. REBOOTING...\"}",
                                              (long long)Sys_Info.epochtime);
            ESP_LOGI("AZURE", "Resetting ESP. Requesting auto-reboot in 5 seconds.");

            Sys_Info.isRebootRequested = true;
            Sys_Info.rebootTimestamp = xTaskGetTickCount();
        }

        else if(_code == CMD_CODE_IDENTIFY_PAUSE_SCHEDULE) // code == 111
        {
            ESP_LOGW("AZURE", "---------- CMD PAUSE SCHEDULE (111) ----------");
            /* Payload CMD 111:
             * Data: { "DeviceId": 11 }  hoặc top-level DeviceId */
            cJSON *DeviceId = cJSON_GetObjectItem(data, "DeviceId");
            if(DeviceId == NULL)
            {
                DeviceId = cJSON_GetObjectItem(payload, "DeviceId");
            }

            if(DeviceId != NULL)
            {
                int shift = prvGetDeviceIndexById((uint16_t)DeviceId->valueint);
                if(shift < 0)
                {
                    response->status = COMMAND_STATUS_BAD_REQUEST;
                    snprintf(response->payload, sizeof(response->payload), "Invalid DeviceId %d", DeviceId->valueint);
                    goto method_done;
                }

                DeviceHandle.Device[shift].isSchedulePaused = true;
                FRAM_SaveDevice((uint8_t)shift);

                response->status = COMMAND_STATUS_OK;
                response->payloadLength = snprintf(response->payload, sizeof(response->payload), "Device %d schedule PAUSED for Manual control.", DeviceId->valueint);
                ESP_LOGI("AZURE", "CMD_CODE 111: Device[%d] (ID %d) schedule paused", shift, DeviceId->valueint);
            }
            else
            {
                response->status = COMMAND_STATUS_BAD_REQUEST;
                snprintf(response->payload, sizeof(response->payload), "Missing DeviceId");
            }
        }

        else if(_code == CMD_CODE_IDENTIFY_RESUME_SCHEDULE) // code == 112
        {
            ESP_LOGW("AZURE", "---------- CMD RESUME SCHEDULE (112) ----------");

            /* Payload CMD 112:
             * Top-level: "DeviceId": 11
             * Data: [ { Schedule_Index, StartTime, EndTime, RunTime, PauseTime, IsFinished }, ... ] */
            cJSON *DeviceId = cJSON_GetObjectItem(payload, "DeviceId");
            if(DeviceId == NULL && data != NULL && !cJSON_IsArray(data))
            {
                DeviceId = cJSON_GetObjectItem(data, "DeviceId");
            }

            if(DeviceId != NULL)
            {
                uint16_t _deviceId = (uint16_t)DeviceId->valueint;
                int shift = prvGetDeviceIndexById(_deviceId);
                if(shift < 0)
                {
                    response->status = COMMAND_STATUS_BAD_REQUEST;
                    snprintf(response->payload, sizeof(response->payload), "Invalid DeviceId %d", _deviceId);
                    goto method_done;
                }

                if(_deviceId == GROUP_3_DEVICE_ID_1)
                {
                    if (Sys_Info.feederMode == 0) // Chỉ bỏ qua ở chế độ 2 động cơ
                    {
                        response->status = COMMAND_STATUS_BAD_REQUEST;
                        snprintf(response->payload, sizeof(response->payload), "Ignore schedule for Feeder_1_M1");
                        goto method_done;
                    }
                }

                Device_Parameters_t *dev = &DeviceHandle.Device[shift];

                if(data != NULL && cJSON_IsArray(data))
                {
                    // Reset toàn bộ danh sách lịch trình cũ trong RAM
                    memset(dev->schedules, 0, sizeof(dev->schedules));
                    dev->scheduleCount = 0;
                    dev->scheduleIndex = 0;
                    dev->isScheduled = false;

                    int arr_size = cJSON_GetArraySize(data);
                    int max_schedules = sizeof(dev->schedules) / sizeof(dev->schedules[0]);
                    if(arr_size > max_schedules) arr_size = max_schedules;

                    for(int i = 0; i < arr_size; i++)
                    {
                        cJSON *item = cJSON_GetArrayItem(data, i);
                        if(item)
                        {
                            cJSON *StartTime  = cJSON_GetObjectItem(item, "StartTime");
                            cJSON *EndTime    = cJSON_GetObjectItem(item, "EndTime");
                            cJSON *IsFinished = cJSON_GetObjectItem(item, "IsFinished");

                            if(StartTime && EndTime)
                            {
                                uint64_t _startTime = (uint64_t)StartTime->valuedouble;
                                uint64_t _stopTime  = (uint64_t)EndTime->valuedouble;
                                uint8_t  _isFinished    = IsFinished ? (uint8_t)IsFinished->valueint : 0;

                                int second = _startTime % 100; _startTime /= 100;
                                int minute = _startTime % 100; _startTime /= 100;
                                int hour   = _startTime % 100; _startTime /= 100;
                                int year   = 2000 + (_startTime % 100); _startTime /= 100;
                                int month  = _startTime % 100; _startTime /= 100;
                                int day    = _startTime % 100;

                                int second_end = _stopTime % 100; _stopTime /= 100;
                                int minute_end = _stopTime % 100; _stopTime /= 100;
                                int hour_end   = _stopTime % 100; _stopTime /= 100;
                                int year_end   = 2000 + (_stopTime % 100); _stopTime /= 100;
                                int month_end  = _stopTime % 100; _stopTime /= 100;
                                int day_end    = _stopTime % 100;

                                struct tm t  = { .tm_year = year - 1900, .tm_mon = month - 1, .tm_mday = day,
                                                 .tm_hour = hour, .tm_min = minute, .tm_sec = second, .tm_isdst = -1 };
                                struct tm te = { .tm_year = year_end - 1900, .tm_mon = month_end - 1, .tm_mday = day_end,
                                                 .tm_hour = hour_end, .tm_min = minute_end, .tm_sec = second_end, .tm_isdst = -1 };

                                time_t epoch     = mktime(&t);
                                time_t epoch_end = mktime(&te);

                                if(epoch >= epoch_end)
                                {
                                    ESP_LOGE("AZURE:", "Invalid schedule parameters item %d. Skipping.", i);
                                    continue;
                                }

                                dev->schedules[dev->scheduleCount].startTime  = epoch;
                                dev->schedules[dev->scheduleCount].stopTime   = epoch_end;
                                dev->schedules[dev->scheduleCount].isFinished = _isFinished;

                                dev->scheduleCount++;
                                dev->isScheduled = true;

                                if(dev->scheduleCount >= max_schedules)
                                {
                                    break;
                                }
                            }
                        }
                    }

                    if(dev->scheduleCount > 0)
                    {
                        // Tìm lịch trình chưa hoàn thành đầu tiên
                        uint8_t active_idx = 0;
                        for(uint8_t k = 0; k < dev->scheduleCount; k++)
                        {
                            if(dev->schedules[k].isFinished == 0)
                            {
                                active_idx = k;
                                break;
                            }
                        }
                        dev->scheduleIndex = active_idx;
                        prvScheduleApplyToDevice(dev, &dev->schedules[active_idx]);
                    }
                }
                else
                {
                    if(dev->scheduleCount > 0)
                    {
                        dev->isScheduled = true;
                        prvScheduleApplyToDevice(dev, &dev->schedules[dev->scheduleIndex]);
                    }
                }

                // Hủy cờ tạm dừng → cho phép lịch trình hoạt động lại
                dev->isSchedulePaused = false;

                FRAM_SaveDevice((uint8_t)shift);

                response->status = COMMAND_STATUS_OK;
                response->payloadLength = snprintf(response->payload, sizeof(response->payload), "Device %d schedule RESUMED with %u schedules", _deviceId, (unsigned int)dev->scheduleCount);

                ESP_LOGI("AZURE", "CMD_CODE 112: Device[%d] (ID %d) schedule resumed with %u items", shift, _deviceId, (unsigned int)dev->scheduleCount);
            }
            else
            {
                response->status = COMMAND_STATUS_BAD_REQUEST;
                snprintf(response->payload, sizeof(response->payload), "Missing DeviceId");
            }
        }

        else if(_code == CMD_CODE_UPDATE_FIRMWARE) //code == 501
        {
            cJSON *Version = cJSON_GetObjectItem(data, "Version");
            cJSON *Url = cJSON_GetObjectItem(data, "Url");

            if((Version != NULL) && cJSON_IsString(Version) && (Url != NULL) && cJSON_IsString(Url))
            {
                ESP_LOGI("AZURE: ", "---------- UPDATE FIRMWARE ----------");
                ESP_LOGI("AZURE: UPDATE FIRMWARE", "Version: %s, Url: %s\n", Version->valuestring, Url->valuestring);

                response->status = COMMAND_STATUS_OK;
                response->payloadLength = snprintf(response->payload, sizeof(response->payload), "Update version %s successfull", Version->valuestring);
                ESP_LOGI("AZURE: UPDATE FIRMWARE", "Preparing to update version %s", Version->valuestring);

                // Execute Telemetry First to Unblock Backend Thread Cleanly
                cJSON *tele = cJSON_CreateObject();
                cJSON *payload = cJSON_CreateObject();
                if(tele != NULL && payload != NULL)
                {
                    char *tele_str;
                    cJSON_AddNumberToObject(tele, "status", 200);
                    cJSON_AddItemToObject(tele, "payload", payload);
                    cJSON_AddNumberToObject(payload, "Code", 501);
                    cJSON_AddNumberToObject(payload, "TimeStamp", (double)Sys_Info.epochtime);
                    cJSON_AddStringToObject(payload, "Message", response->payload);

                    tele_str = cJSON_PrintUnformatted(tele);
                    if(tele_str != NULL)
                    {
                        PushTelemetry(tele_str);
                        free(tele_str);
                    }
                    else
                    {
                        ESP_LOGE("AZURE: CMD CALLBACK", "Telemetry json build failed");
                    }
                    ESP_LOGI("AZURE: UPDATE FIRMWARE", "Firmware dispatching to existing user_ota.c Daemon task...");
                }
                else
                {
                    ESP_LOGE("AZURE: CMD CALLBACK", "Telemetry alloc failed");
                }
                
                if(tele != NULL)
                {
                    cJSON_Delete(tele);
                }
                else if(payload != NULL)
                {
                    cJSON_Delete(payload);
                }

                // Save URL and Signal event group for User_OTA_Task
                memset(g_ota_update_url, 0, sizeof(g_ota_update_url));
                strncpy(g_ota_update_url, Url->valuestring, sizeof(g_ota_update_url) - 1);
                
                if(otaEventGroup != NULL)
                {
                    bIsOtaActivated = true;
                    IoTHubHandle.isNeedReinit = true;
                    xEventGroupSetBits(otaEventGroup, OTA_WAIT_BIT);
                }

            }
            else
            {
                response->status = COMMAND_STATUS_BAD_REQUEST;
                response->payloadLength = sprintf(response->payload, "Missing Version/Url");
                ESP_LOGE("AZURE: UPDATE FIRMWARE", "Missing Version or Url field");
            }
        }

        else if(_code == CMD_CODE_ASK_VERSION) //code == 502
        {
            ESP_LOGI("AZURE: ", "---------- ASK FOR VERSION ? ----------");

            response->status = COMMAND_STATUS_OK;
            response->payloadLength = sprintf(response->payload, "Firmware Version: %s", esp_app_get_description()->version);

            time_t now = Sys_Info.epochtime;
            char formatted_now[32];
            char msg_str[64];

            prvFormatScheduleTime(now, formatted_now, sizeof(formatted_now));

            snprintf(msg_str, sizeof(msg_str), "Version %s ", esp_app_get_description()->version);

            cJSON *tele = cJSON_CreateObject();
            cJSON *payload = cJSON_CreateObject();
            if(tele != NULL && payload != NULL)
            {
                cJSON_AddItemToObject(tele, "payload", payload);
                cJSON_AddNumberToObject(payload, "Code", CMD_CODE_ASK_VERSION);
                cJSON_AddStringToObject(payload, "Time", formatted_now);
                cJSON_AddStringToObject(payload, "Message", msg_str);

                char *tele_str = cJSON_PrintUnformatted(tele);
                if(tele_str != NULL)
                {
                    ESP_LOGI("AZURE: ASK VERSION", "Push version telemetry: %s", tele_str);
                    PushTelemetry(tele_str);
                    free(tele_str);
                }
                else
                {
                    ESP_LOGE("AZURE: ASK VERSION", "Telemetry json build failed");
                }
            }
            else
            {
                ESP_LOGE("AZURE: ASK VERSION", "Telemetry alloc failed");
            }
            if(tele != NULL)
            {
                cJSON_Delete(tele);
            }
            else if(payload != NULL)
            {
                cJSON_Delete(payload);
            }
        }

        else if(_code == CMD_CODE_ASK_RESETCOUNT) //code == 503
        {
            ESP_LOGI("AZURE", "---------- ASK FOR RESET COUNT ? ----------");

            response->status = COMMAND_STATUS_OK;
            response->payloadLength = sprintf(response->payload, "RESET COUNT: %u", (unsigned)reset_count);
            ESP_LOGI("AZURE: RESET COUNT", "Reset count = %u", (unsigned)reset_count);

            time_t now = Sys_Info.epochtime;

            cJSON *tele503 = cJSON_CreateObject();
            cJSON *payload503 = cJSON_CreateObject();
            if(tele503 != NULL && payload503 != NULL)
            {
                cJSON_AddItemToObject(tele503, "payload", payload503);
                cJSON_AddNumberToObject(payload503, "Code", CMD_CODE_ASK_RESETCOUNT);
                cJSON_AddNumberToObject(payload503, "TimeStamp", (double)now);
                cJSON_AddStringToObject(payload503, "Message", response->payload);

                char *tele_str503 = cJSON_PrintUnformatted(tele503);
                if(tele_str503 != NULL)
                {
                    ESP_LOGI("AZURE: RESET COUNT", "Push telemetry: %s", tele_str503);
                    PushTelemetry(tele_str503);
                    free(tele_str503);
                }
                else
                {
                    ESP_LOGE("AZURE: RESET COUNT", "Telemetry json build failed");
                }
            }
            else
            {
                ESP_LOGE("AZURE: RESET COUNT", "Telemetry alloc failed");
                if(payload503 != NULL) cJSON_Delete(payload503);
            }
            if(tele503 != NULL) cJSON_Delete(tele503);
        }

        else if(_code == CMD_CODE_ASK_RUNNING_TIME) //code == 504
        {
            ESP_LOGI("AZURE", "---------- ASK FOR RUNNING TIME ? ----------");

            int64_t uptime_us = esp_timer_get_time(); // microseconds since boot
            if(uptime_us < 0)
            {
                response->status = COMMAND_STATUS_BAD_REQUEST;
                response->payloadLength = sprintf(response->payload, "Error: FAIL TO LOAD RUNNING TIME");
                ESP_LOGE("AZURE: RUNNING TIME", "esp_timer_get_time() returned invalid value");
            }
            else
            {
                uint32_t total_sec = (uint32_t)(uptime_us / 1000000ULL);
                uint32_t hours     = total_sec / 3600;
                uint32_t minutes   = (total_sec % 3600) / 60;
                uint32_t seconds   = total_sec % 60;

                char running_time_str[32];
                snprintf(running_time_str, sizeof(running_time_str), "%u:%02u:%02u", (unsigned int)hours, (unsigned int)minutes, (unsigned int)seconds);

                response->status = COMMAND_STATUS_OK;
                response->payloadLength = sprintf(response->payload, "RUNNING TIME: %s", running_time_str);
                ESP_LOGI("AZURE: RUNNING TIME", "Uptime = %s", running_time_str);

                time_t now = Sys_Info.epochtime;

                cJSON *tele504 = cJSON_CreateObject();
                cJSON *payload504 = cJSON_CreateObject();
                if(tele504 != NULL && payload504 != NULL)
                {
                    cJSON_AddItemToObject(tele504, "payload", payload504);
                    cJSON_AddNumberToObject(payload504, "Code", CMD_CODE_ASK_RUNNING_TIME);
                    cJSON_AddNumberToObject(payload504, "TimeStamp", (double)now);
                    cJSON_AddStringToObject(payload504, "Message", response->payload);

                    char *tele_str504 = cJSON_PrintUnformatted(tele504);
                    if(tele_str504 != NULL)
                    {
                        ESP_LOGI("AZURE: RUNNING TIME", "Push telemetry: %s", tele_str504);
                        PushTelemetry(tele_str504);
                        free(tele_str504);
                    }
                    else
                    {
                        ESP_LOGE("AZURE: RUNNING TIME", "Telemetry json build failed");
                    }
                }
                else
                {
                    ESP_LOGE("AZURE: RUNNING TIME", "Telemetry alloc failed");
                    if(payload504 != NULL) cJSON_Delete(payload504);
                }
                if(tele504 != NULL) cJSON_Delete(tele504);
            }
        }

        else if(_code == CMD_CODE_ASK_IP_WEB_PORTAL ) //code == 505
        {
            ESP_LOGI("AZURE", "---------- ASK FOR IP CONFIG (505) ----------");

            char ip_str[16] = {0};
            Fram_Read_Data(FRAM_IP_ADDR_ADDR, (uint8_t *)ip_str, 16);
            ip_str[15] = '\0';
            
            if (ip_str[0] == '\0' || (uint8_t)ip_str[0] == 0xFF)
            {
                strcpy(ip_str, "0.0.0.0");
            }

            response->status = COMMAND_STATUS_OK;
            response->payloadLength = snprintf(response->payload, sizeof(response->payload),
                "{\"Code\":505,\"TimeStamp\":%lld,\"IP\":\"%s\"}",
                (long long)Sys_Info.epochtime, ip_str);
            ESP_LOGI("AZURE: ASK IP CONFIG", "IP Address = %s", ip_str);

            time_t now = Sys_Info.epochtime;
            char formatted_now[32];
            prvFormatScheduleTime(now, formatted_now, sizeof(formatted_now));

            cJSON *tele505 = cJSON_CreateObject();
            cJSON *payload505 = cJSON_CreateObject();
            if(tele505 != NULL && payload505 != NULL)
            {
                cJSON_AddItemToObject(tele505, "payload", payload505);
                cJSON_AddNumberToObject(payload505, "Code", CMD_CODE_ASK_IP_WEB_PORTAL);
                cJSON_AddNumberToObject(payload505, "TimeStamp", (double)now);
                cJSON_AddStringToObject(payload505, "Time", formatted_now);
                cJSON_AddStringToObject(payload505, "Message", ip_str);
                cJSON_AddStringToObject(payload505, "IP", ip_str);

                char *tele_str505 = cJSON_PrintUnformatted(tele505);
                if(tele_str505 != NULL)
                {
                    ESP_LOGI("AZURE: ASK IP CONFIG", "Push telemetry: %s", tele_str505);
                    PushTelemetry(tele_str505);
                    free(tele_str505);
                }
                else
                {
                    ESP_LOGE("AZURE: ASK IP CONFIG", "Telemetry json build failed");
                }
            }
            if(tele505 != NULL)
            {
                cJSON_Delete(tele505);
            }
            else if(payload505 != NULL)
            {
                cJSON_Delete(payload505);
            }
        }

        else if(_code == CMD_CODE_FEEDER_CONTACTOR_POND) // code == 115
        {
            ESP_LOGI("AZURE", "---------- CONFIGURE FEEDER MODE (115) ----------");
            if (Sys_Info.pondMode == POND_MODE_4_DEV)
            {
                response->status = COMMAND_STATUS_BAD_REQUEST;
                response->payloadLength = snprintf(response->payload, sizeof(response->payload),
                                                   "Ignore Feeder Mode config (Code 115) in Pond Mode 4");
                ESP_LOGW("AZURE", "Ignore Feeder Mode config (Code 115) in Pond Mode 4");
                goto method_done;
            }

            cJSON *feederMode = cJSON_GetObjectItem(data, "FeederMode");
            cJSON *activeDeviceID = cJSON_GetObjectItem(data, "ActiveDeviceID");
            if (feederMode != NULL && activeDeviceID != NULL)
            {
                uint8_t mode = (uint8_t)feederMode->valueint;
                uint16_t actId = (uint16_t)activeDeviceID->valueint;
                // Logic kiểm tra tham số:
                // Nếu mode = 0, ép activeFeederId về 99.
                // Nếu mode = 1, activeFeederId bắt buộc phải là 31 hoặc 32.
                if (mode == 0)
                {
                    actId = 99;
                }
                if ((mode == 0 || mode == 1) && (actId == 31 || actId == 32 || actId == 99))
                {
                    Sys_Info.feederMode = mode;
                    Sys_Info.activeFeederId = actId;
                    // Lưu cấu hình vào FRAM
                    Fram_Write_Data(FRAM_FEEDER_MODE_ADDR, &mode, 1);
                    uint8_t id_buf[2];
                    id_buf[0] = (uint8_t)(actId >> 8);
                    id_buf[1] = (uint8_t)(actId & 0xFF);
                    Fram_Write_Data(FRAM_FEEDER_ACTIVE_ID_ADDR, id_buf, 2);
                    response->status = COMMAND_STATUS_OK;
                    response->payloadLength = snprintf(response->payload, sizeof(response->payload), "Feeder config set: Mode=%d, ActiveDeviceID=%d", mode, actId);
                    ESP_LOGI("AZURE", "Feeder config updated: Mode=%d, ActiveDeviceID=%d", mode, actId);
                    // Gửi Telemetry báo cáo trạng thái mới về Server
                    time_t now = Sys_Info.epochtime;
                    cJSON *tele115 = cJSON_CreateObject();
                    cJSON *payload115 = cJSON_CreateObject();
                    if(tele115 != NULL && payload115 != NULL)
                    {
                        cJSON_AddItemToObject(tele115, "payload", payload115);
                        cJSON_AddNumberToObject(payload115, "Code", CMD_CODE_FEEDER_CONTACTOR_POND);
                        cJSON_AddNumberToObject(payload115, "TimeStamp", (double)now);
                        cJSON_AddStringToObject(payload115, "Message", response->payload);
                        char *tele_str115 = cJSON_PrintUnformatted(tele115);
                        if(tele_str115 != NULL)
                        {
                            ESP_LOGI("AZURE", "Push telemetry 115: %s", tele_str115);
                            PushTelemetry(tele_str115);
                            free(tele_str115);
                        }
                        else
                        {
                            ESP_LOGE("AZURE", "Telemetry 115 json build failed");
                        }
                    }
                    else
                    {
                        ESP_LOGE("AZURE", "Telemetry 115 alloc failed");
                        if(payload115 != NULL) cJSON_Delete(payload115);
                    }
                    if(tele115 != NULL) cJSON_Delete(tele115);
                }
                else
                {
                    response->status = COMMAND_STATUS_BAD_REQUEST;
                    response->payloadLength = snprintf(response->payload, sizeof(response->payload), "Invalid config values: Mode must be 0/1, ActiveDeviceID must be 31/32 for Mode 1");
                    ESP_LOGE("AZURE", "Feeder config invalid: Mode=%d, ActiveID=%d", mode, actId);
                }
            }
            else
            {
                response->status = COMMAND_STATUS_BAD_REQUEST;
                response->payloadLength = snprintf(response->payload, sizeof(response->payload), "Missing FeederMode or ActiveDeviceID");
                ESP_LOGE("AZURE", "Feeder config failed: Missing fields");
            }
        }

        else if(_code == CMD_CODE_OXY_POND) // code == 114
        {
            ESP_LOGI("AZURE", "---------- CONFIGURE OXY MODE (114) ----------");
            if (Sys_Info.pondMode == POND_MODE_4_DEV)
            {
                response->status = COMMAND_STATUS_BAD_REQUEST;
                response->payloadLength = snprintf(response->payload, sizeof(response->payload), "Ignore Oxy Mode config (Code 114) in Pond Mode 4");
                ESP_LOGW("AZURE", "Ignore Oxy Mode config (Code 114) in Pond Mode 4");
                goto method_done;
            }

            cJSON *oxyMode = cJSON_GetObjectItem(data, "OxyMode");
            cJSON *activeDeviceID = cJSON_GetObjectItem(data, "ActiveDeviceID");
            if (oxyMode != NULL && activeDeviceID != NULL)
            {
                uint8_t mode = (uint8_t)oxyMode->valueint;
                uint16_t actId = (uint16_t)activeDeviceID->valueint;
                // Logic kiểm tra tham số:
                // Nếu mode = 0, ép activeOxyId về 99.
                // Nếu mode = 1, activeOxyId bắt buộc phải là 21 hoặc 22.
                if (mode == 0)
                {
                    actId = 99;
                }
                if ((mode == 0 || mode == 1) && (actId == 21 || actId == 22 || actId == 99))
                {
                    Sys_Info.oxyMode = mode;
                    Sys_Info.activeOxyId = actId;
                    // Lưu cấu hình vào FRAM
                    Fram_Write_Data(FRAM_OXY_MODE_ADDR, &mode, 1);
                    uint8_t id_buf[2];
                    id_buf[0] = (uint8_t)(actId >> 8);
                    id_buf[1] = (uint8_t)(actId & 0xFF);
                    Fram_Write_Data(FRAM_OXY_ACTIVE_ID_ADDR, id_buf, 2);
                    response->status = COMMAND_STATUS_OK;
                    response->payloadLength = snprintf(response->payload, sizeof(response->payload), "Oxy config set: Mode=%d, ActiveDeviceID=%d", mode, actId);
                    ESP_LOGI("AZURE", "Oxy config updated: Mode=%d, ActiveDeviceID=%d", mode, actId);
                    // Gửi Telemetry báo cáo trạng thái mới về Server
                    time_t now = Sys_Info.epochtime;
                    cJSON *tele114 = cJSON_CreateObject();
                    cJSON *payload114 = cJSON_CreateObject();
                    if(tele114 != NULL && payload114 != NULL)
                    {
                        cJSON_AddItemToObject(tele114, "payload", payload114);
                        cJSON_AddNumberToObject(payload114, "Code", CMD_CODE_OXY_POND);
                        cJSON_AddNumberToObject(payload114, "TimeStamp", (double)now);
                        cJSON_AddStringToObject(payload114, "Message", response->payload);
                        char *tele_str114 = cJSON_PrintUnformatted(tele114);
                        if(tele_str114 != NULL)
                        {
                            ESP_LOGI("AZURE", "Push telemetry 114: %s", tele_str114);
                            PushTelemetry(tele_str114);
                            free(tele_str114);
                        }
                        else
                        {
                            ESP_LOGE("AZURE", "Telemetry 114 json build failed");
                        }
                    }
                    else
                    {
                        ESP_LOGE("AZURE", "Telemetry 114 alloc failed");
                        if(payload114 != NULL) cJSON_Delete(payload114);
                    }
                    if(tele114 != NULL) cJSON_Delete(tele114);
                }
                else
                {
                    response->status = COMMAND_STATUS_BAD_REQUEST;
                    response->payloadLength = snprintf(response->payload, sizeof(response->payload), "Invalid config values: Mode must be 0/1, ActiveDeviceID must be 21/22 for Mode 1");
                    ESP_LOGE("AZURE", "Oxy config invalid: Mode=%d, ActiveID=%d", mode, actId);
                }
            }
            else
            {
                response->status = COMMAND_STATUS_BAD_REQUEST;
                response->payloadLength = snprintf(response->payload, sizeof(response->payload), "Missing OxyMode or ActiveDeviceID");
                ESP_LOGE("AZURE", "Oxy config failed: Missing fields");
            }
        }

        /* ================================================================
         * VFD Check – Reject all VFD commands (200-211) if VFD is disabled
         * ================================================================ */
        else if (_code >= 200 && _code <= 211 && Sys_Info.vfdEnabled == 0)
        {
            response->status = COMMAND_STATUS_BAD_REQUEST;
            response->payloadLength = snprintf(response->payload, sizeof(response->payload),
                "{\"Code\":%d,\"TimeStamp\":%lld,\"Message\":\"VFD is disabled in system configuration\"}",
                _code, (long long)Sys_Info.epochtime);
            ESP_LOGW("AZURE", "VFD command %d ignored because VFD is disabled", _code);
        }

        /* ================================================================
         * CMD 200: RUN FORWARD – Chay thuan bien tan
         * Payload: { "Code": 200, "TimeStamp": ... }
         * Khong can truong Data
         * ================================================================ */
        else if(_code == CMD_CODE_RUN_FORWARD) // code == 200
        {
            ESP_LOGI("AZURE", "---------- CMD VFD RUN FORWARD (200) ----------");
            RS485_Status_t ret = GD200A_RunForward(GD200A_SLAVE_ADDR);
            if(ret == RS485_OK)
            {
                response->status = COMMAND_STATUS_OK;
                response->payloadLength = snprintf(response->payload, sizeof(response->payload),
                    "{\"Code\":200,\"TimeStamp\":%lld,\"Message\":\"VFD RUNNING FORWARD OK\"}",
                    (long long)Sys_Info.epochtime);
                ESP_LOGI("AZURE", "CMD 200: VFD run forward OK");
            }
            else
            {
                response->status = COMMAND_STATUS_BAD_REQUEST;
                response->payloadLength = snprintf(response->payload, sizeof(response->payload),
                    "{\"Code\":200,\"TimeStamp\":%lld,\"Message\":\"VFD RUN FORWARD FAIL (Modbus err %d)\"}",
                    (long long)Sys_Info.epochtime, (int)ret);
                ESP_LOGE("AZURE", "CMD 200: VFD run forward FAIL, Modbus status=%d", (int)ret);
            }
        }

        /* ================================================================
         * CMD 201: RUN REVERSE – Chay nghich bien tan
         * Payload: { "Code": 201, "TimeStamp": ... }
         * ================================================================ */
        else if(_code == CMD_CODE_RUN_REVERSE) // code == 201
        {
            ESP_LOGI("AZURE", "---------- CMD VFD RUN REVERSE (201) ----------");
            RS485_Status_t ret = GD200A_RunReverse(GD200A_SLAVE_ADDR);
            if(ret == RS485_OK)
            {
                response->status = COMMAND_STATUS_OK;
                response->payloadLength = snprintf(response->payload, sizeof(response->payload),
                    "{\"Code\":201,\"TimeStamp\":%lld,\"Message\":\"VFD RUNNING REVERSE OK\"}",
                    (long long)Sys_Info.epochtime);
                ESP_LOGI("AZURE", "CMD 201: VFD run reverse OK");
            }
            else
            {
                response->status = COMMAND_STATUS_BAD_REQUEST;
                response->payloadLength = snprintf(response->payload, sizeof(response->payload),
                    "{\"Code\":201,\"TimeStamp\":%lld,\"Message\":\"VFD RUN REVERSE FAIL (Modbus err %d)\"}",
                    (long long)Sys_Info.epochtime, (int)ret);
                ESP_LOGE("AZURE", "CMD 201: VFD run reverse FAIL, Modbus status=%d", (int)ret);
            }
        }

        /* ================================================================
         * CMD 202: STOP – Dung bien tan (Soft Stop – giam toc theo DEC time)
         * Payload: { "Code": 202, "TimeStamp": ... }
         * ================================================================ */
        else if(_code == CMD_CODE_STOP) // code == 202
        {
            ESP_LOGI("AZURE", "---------- CMD VFD STOP (202) ----------");
            RS485_Status_t ret = GD200A_Stop(GD200A_SLAVE_ADDR);
            if(ret == RS485_OK)
            {
                response->status = COMMAND_STATUS_OK;
                response->payloadLength = snprintf(response->payload, sizeof(response->payload),
                    "{\"Code\":202,\"TimeStamp\":%lld,\"Message\":\"VFD STOP OK\"}",
                    (long long)Sys_Info.epochtime);
                ESP_LOGI("AZURE", "CMD 202: VFD stop OK");
            }
            else
            {
                response->status = COMMAND_STATUS_BAD_REQUEST;
                response->payloadLength = snprintf(response->payload, sizeof(response->payload),
                    "{\"Code\":202,\"TimeStamp\":%lld,\"Message\":\"VFD STOP FAIL (Modbus err %d)\"}",
                    (long long)Sys_Info.epochtime, (int)ret);
                ESP_LOGE("AZURE", "CMD 202: VFD stop FAIL, Modbus status=%d", (int)ret);
            }
        }

        /* ================================================================
         * CMD 203: SET SOFT START TIME – Cai dat thoi gian tang toc (Accel)
         * Payload: { "Code": 203, "TimeStamp": ..., "Data": { "AccelTime": 5.0 } }
         * AccelTime: don vi giay (0.0 – 3600.0)
         * ================================================================ */
        else if(_code == CMD_CODE_SET_SOFT_START_TIME) // code == 203
        {
            ESP_LOGI("AZURE", "---------- CMD VFD SET SOFT START TIME (203) ----------");
            cJSON *AccelTime = cJSON_GetObjectItem(data, "AccelTime");
            if(AccelTime != NULL && (cJSON_IsNumber(AccelTime)))
            {
                float accel_s = (float)AccelTime->valuedouble;
                if(accel_s < 0.0f || accel_s > 3600.0f)
                {
                    response->status = COMMAND_STATUS_BAD_REQUEST;
                    response->payloadLength = snprintf(response->payload, sizeof(response->payload),
                        "{\"Code\":203,\"TimeStamp\":%lld,\"Message\":\"AccelTime out of range (0.0-3600.0s)\"}",
                        (long long)Sys_Info.epochtime);
                    ESP_LOGE("AZURE", "CMD 203: AccelTime=%.1f out of range", accel_s);
                }
                else
                {
                    RS485_Status_t ret = GD200A_SetAccelTime(GD200A_SLAVE_ADDR, accel_s);
                    if(ret == RS485_OK)
                    {
                        response->status = COMMAND_STATUS_OK;
                        response->payloadLength = snprintf(response->payload, sizeof(response->payload),
                            "{\"Code\":203,\"TimeStamp\":%lld,\"Message\":\"VFD SET ACCEL TIME %.1fs OK\"}",
                            (long long)Sys_Info.epochtime, accel_s);
                        ESP_LOGI("AZURE", "CMD 203: Set accel time=%.1fs OK", accel_s);
                    }
                    else
                    {
                        response->status = COMMAND_STATUS_BAD_REQUEST;
                        response->payloadLength = snprintf(response->payload, sizeof(response->payload),
                            "{\"Code\":203,\"TimeStamp\":%lld,\"Message\":\"SET ACCEL TIME FAIL (Modbus err %d)\"}",
                            (long long)Sys_Info.epochtime, (int)ret);
                        ESP_LOGE("AZURE", "CMD 203: Set accel time FAIL, Modbus status=%d", (int)ret);
                    }
                }
            }
            else
            {
                response->status = COMMAND_STATUS_BAD_REQUEST;
                response->payloadLength = snprintf(response->payload, sizeof(response->payload),
                    "{\"Code\":203,\"TimeStamp\":%lld,\"Message\":\"Missing AccelTime field (float, unit: seconds)\"}",
                    (long long)Sys_Info.epochtime);
                ESP_LOGE("AZURE", "CMD 203: Missing AccelTime field");
            }
        }

        /* ================================================================
         * CMD 204: SET SOFT STOP TIME – Cai dat thoi gian giam toc (Decel)
         * Payload: { "Code": 204, "TimeStamp": ..., "Data": { "DecelTime": 10.0 } }
         * DecelTime: don vi giay (0.0 – 3600.0)
         * ================================================================ */
        else if(_code == CMD_CODE_SET_SOFT_STOP_TIME) // code == 204
        {
            ESP_LOGI("AZURE", "---------- CMD VFD SET SOFT STOP TIME (204) ----------");
            cJSON *DecelTime = cJSON_GetObjectItem(data, "DecelTime");
            if(DecelTime != NULL && (cJSON_IsNumber(DecelTime)))
            {
                float decel_s = (float)DecelTime->valuedouble;
                if(decel_s < 0.0f || decel_s > 3600.0f)
                {
                    response->status = COMMAND_STATUS_BAD_REQUEST;
                    response->payloadLength = snprintf(response->payload, sizeof(response->payload),
                        "{\"Code\":204,\"TimeStamp\":%lld,\"Message\":\"DecelTime out of range (0.0-3600.0s)\"}",
                        (long long)Sys_Info.epochtime);
                    ESP_LOGE("AZURE", "CMD 204: DecelTime=%.1f out of range", decel_s);
                }
                else
                {
                    RS485_Status_t ret = GD200A_SetDecelTime(GD200A_SLAVE_ADDR, decel_s);
                    if(ret == RS485_OK)
                    {
                        response->status = COMMAND_STATUS_OK;
                        response->payloadLength = snprintf(response->payload, sizeof(response->payload),
                            "{\"Code\":204,\"TimeStamp\":%lld,\"Message\":\"VFD SET DECEL TIME %.1fs OK\"}",
                            (long long)Sys_Info.epochtime, decel_s);
                        ESP_LOGI("AZURE", "CMD 204: Set decel time=%.1fs OK", decel_s);
                    }
                    else
                    {
                        response->status = COMMAND_STATUS_BAD_REQUEST;
                        response->payloadLength = snprintf(response->payload, sizeof(response->payload),
                            "{\"Code\":204,\"TimeStamp\":%lld,\"Message\":\"SET DECEL TIME FAIL (Modbus err %d)\"}",
                            (long long)Sys_Info.epochtime, (int)ret);
                        ESP_LOGE("AZURE", "CMD 204: Set decel time FAIL, Modbus status=%d", (int)ret);
                    }
                }
            }
            else
            {
                response->status = COMMAND_STATUS_BAD_REQUEST;
                response->payloadLength = snprintf(response->payload, sizeof(response->payload),
                    "{\"Code\":204,\"TimeStamp\":%lld,\"Message\":\"Missing DecelTime field (float, unit: seconds)\"}",
                    (long long)Sys_Info.epochtime);
                ESP_LOGE("AZURE", "CMD 204: Missing DecelTime field");
            }
        }

        /* ================================================================
         * CMD 205: SET FREQUENCY – Cai dat tan so bien tan
         * Payload: { "Code": 205, "TimeStamp": ..., "Data": { "Frequency": 45.5 } }
         * Frequency: don vi Hz (0.00 – 400.00)
         * ================================================================ */
        else if(_code == CMD_CODE_SET_FREQUENCE) // code == 205
        {
            ESP_LOGI("AZURE", "---------- CMD VFD SET FREQUENCY (205) ----------");
            cJSON *Frequency = cJSON_GetObjectItem(data, "Frequency");
            if(Frequency != NULL && (cJSON_IsNumber(Frequency)))
            {
                float freq_hz = (float)Frequency->valuedouble;
                if(freq_hz < 0.0f || freq_hz > 400.0f)
                {
                    response->status = COMMAND_STATUS_BAD_REQUEST;
                    response->payloadLength = snprintf(response->payload, sizeof(response->payload),
                        "{\"Code\":205,\"TimeStamp\":%lld,\"Message\":\"Frequency out of range (0.0-400.0 Hz)\"}",
                        (long long)Sys_Info.epochtime);
                    ESP_LOGE("AZURE", "CMD 205: Frequency=%.2f out of range", freq_hz);
                }
                else
                {
                    RS485_Status_t ret = GD200A_SetFrequency(GD200A_SLAVE_ADDR, freq_hz);
                    if(ret == RS485_OK)
                    {
                        response->status = COMMAND_STATUS_OK;
                        response->payloadLength = snprintf(response->payload, sizeof(response->payload),
                            "{\"Code\":205,\"TimeStamp\":%lld,\"Message\":\"VFD SET FREQUENCY %.2fHz OK\"}",
                            (long long)Sys_Info.epochtime, freq_hz);
                        ESP_LOGI("AZURE", "CMD 205: Set frequency=%.2fHz OK", freq_hz);
                    }
                    else
                    {
                        response->status = COMMAND_STATUS_BAD_REQUEST;
                        response->payloadLength = snprintf(response->payload, sizeof(response->payload),
                            "{\"Code\":205,\"TimeStamp\":%lld,\"Message\":\"SET FREQUENCY FAIL (Modbus err %d)\"}",
                            (long long)Sys_Info.epochtime, (int)ret);
                        ESP_LOGE("AZURE", "CMD 205: Set frequency FAIL, Modbus status=%d", (int)ret);
                    }
                }
            }
            else
            {
                response->status = COMMAND_STATUS_BAD_REQUEST;
                response->payloadLength = snprintf(response->payload, sizeof(response->payload),
                    "{\"Code\":205,\"TimeStamp\":%lld,\"Message\":\"Missing Frequency field (float, unit: Hz)\"}",
                    (long long)Sys_Info.epochtime);
                ESP_LOGE("AZURE", "CMD 205: Missing Frequency field");
            }
        }

        /* ================================================================
         * CMD 206: ENABLE OPTIMIZE FOR SHRIMP FARM – Cấu hình tối ưu ao tôm
         * Payload: { "Code": 206, "TimeStamp": ..., "Data": {} }
         * ================================================================ */
        else if(_code == CMD_CODE_ENABLE_OPTIMIZE_VFD) // code == 206
        {
            ESP_LOGI("AZURE", "---------- CMD VFD SHRIMP OPTIMIZATION ENABLE (206) ----------");
            RS485_Status_t ret = GD200A_EnableOptimizeForShrimpFarm(GD200A_SLAVE_ADDR);
            if(ret == RS485_OK)
            {
                response->status = COMMAND_STATUS_OK;
                response->payloadLength = snprintf(response->payload, sizeof(response->payload),
                    "{\"Code\":206,\"TimeStamp\":%lld,\"Message\":\"VFD SHRIMP OPTIMIZATION ENABLED SUCCESSFUL\"}",
                    (long long)Sys_Info.epochtime);
                ESP_LOGI("AZURE", "CMD 206: VFD optimization enable OK");
            }
            else
            {
                response->status = COMMAND_STATUS_BAD_REQUEST;
                response->payloadLength = snprintf(response->payload, sizeof(response->payload),
                    "{\"Code\":206,\"TimeStamp\":%lld,\"Message\":\"VFD OPTIMIZATION ENABLE FAIL (Modbus err %d)\"}",
                    (long long)Sys_Info.epochtime, (int)ret);
                ESP_LOGE("AZURE", "CMD 206: VFD optimization enable FAIL, Modbus status=%d", (int)ret);
            }
        }

        /* ================================================================
         * CMD 207: DISABLE OPTIMIZE FOR SHRIMP FARM – Tắt cấu hình tối ưu ao tôm
         * Payload: { "Code": 207, "TimeStamp": ..., "Data": {} }
         * ================================================================ */
        else if(_code == CMD_CODE_DISABLE_OPTIMIZE_VFD) // code == 207
        {
            ESP_LOGI("AZURE", "---------- CMD VFD SHRIMP OPTIMIZATION DISABLE (207) ----------");
            RS485_Status_t ret = GD200A_DisableOptimizeForShrimpFarm(GD200A_SLAVE_ADDR);
            if(ret == RS485_OK)
            {
                response->status = COMMAND_STATUS_OK;
                response->payloadLength = snprintf(response->payload, sizeof(response->payload),
                    "{\"Code\":207,\"TimeStamp\":%lld,\"Message\":\"VFD SHRIMP OPTIMIZATION DISABLED SUCCESSFUL\"}",
                    (long long)Sys_Info.epochtime);
                ESP_LOGI("AZURE", "CMD 207: VFD optimization disable OK");
            }
            else
            {
                response->status = COMMAND_STATUS_BAD_REQUEST;
                response->payloadLength = snprintf(response->payload, sizeof(response->payload),
                    "{\"Code\":207,\"TimeStamp\":%lld,\"Message\":\"VFD OPTIMIZATION DISABLE FAIL (Modbus err %d)\"}",
                    (long long)Sys_Info.epochtime, (int)ret);
                ESP_LOGE("AZURE", "CMD 207: VFD optimization disable FAIL, Modbus status=%d", (int)ret);
            }
        }

        /* ================================================================
         * CMD 208: CONTROL VFD MANUAL – Chuyển đổi sang Manual (Keypad)
         * Payload: { "Code": 208, "TimeStamp": ..., "Data": {} }
         * ================================================================ */
        else if(_code == CMD_CODE_CONTROL_VFD_MANUAL) // code == 208
        {
            ESP_LOGI("AZURE", "---------- CMD VFD CONTROL MANUAL (208) ----------");
            RS485_Status_t ret = GD200A_SetControlManual(GD200A_SLAVE_ADDR);
            if(ret == RS485_OK)
            {
                response->status = COMMAND_STATUS_OK;
                response->payloadLength = snprintf(response->payload, sizeof(response->payload),
                    "{\"Code\":208,\"TimeStamp\":%lld,\"Message\":\"VFD SWITCHED TO MANUAL OK\"}",
                    (long long)Sys_Info.epochtime);
                ESP_LOGI("AZURE", "CMD 208: VFD Manual OK");
            }
            else
            {
                response->status = COMMAND_STATUS_BAD_REQUEST;
                response->payloadLength = snprintf(response->payload, sizeof(response->payload),
                    "{\"Code\":208,\"TimeStamp\":%lld,\"Message\":\"VFD MANUAL SWITCH FAIL (Modbus err %d)\"}",
                    (long long)Sys_Info.epochtime, (int)ret);
                ESP_LOGE("AZURE", "CMD 208: VFD Manual FAIL, Modbus status=%d", (int)ret);
            }
        }

        /* ================================================================
         * CMD 209: CONTROL VFD AUTO – Chuyển đổi sang Auto (Modbus)
         * Payload: { "Code": 209, "TimeStamp": ..., "Data": {} }
         * ================================================================ */
        else if(_code == CMD_CODE_CONTROL_VFD_AUTO) // code == 209
        {
            ESP_LOGI("AZURE", "---------- CMD VFD CONTROL AUTO (209) ----------");
            RS485_Status_t ret = GD200A_SetControlAuto(GD200A_SLAVE_ADDR);
            if(ret == RS485_OK)
            {
                response->status = COMMAND_STATUS_OK;
                response->payloadLength = snprintf(response->payload, sizeof(response->payload),
                    "{\"Code\":209,\"TimeStamp\":%lld,\"Message\":\"VFD SWITCHED TO AUTO OK\"}",
                    (long long)Sys_Info.epochtime);
                ESP_LOGI("AZURE", "CMD 209: VFD Auto OK");
            }
            else
            {
                response->status = COMMAND_STATUS_BAD_REQUEST;
                response->payloadLength = snprintf(response->payload, sizeof(response->payload),
                    "{\"Code\":209,\"TimeStamp\":%lld,\"Message\":\"VFD AUTO SWITCH FAIL (Modbus err %d)\"}",
                    (long long)Sys_Info.epochtime, (int)ret);
                ESP_LOGE("AZURE", "CMD 209: VFD Auto FAIL, Modbus status=%d", (int)ret);
            }
        }
        
        /* ================================================================
         * CMD 210: SET FUNCTION CODE – Cài đặt một tham số bất kỳ của biến tần (Pxx.yy)
         * Payload: { "Code": 210, "TimeStamp": ..., "Data": { "FunctionCode": "P07.02", "Value": 6 } }
         * ================================================================ */
        else if(_code == CMD_CODE_SET_FUNCTION_CODE) // code == 210
        {
            ESP_LOGI("AZURE", "---------- CMD VFD SET FUNCTION CODE (210) ----------");
            cJSON *Value = cJSON_GetObjectItem(data, "Value");
            if(Value != NULL && cJSON_IsNumber(Value))
            {
                uint16_t val = (uint16_t)Value->valueint;
                uint16_t reg_addr = 0;
                bool valid = false;

                cJSON *FunctionCode = cJSON_GetObjectItem(data, "FunctionCode");
                if(FunctionCode != NULL)
                {
                    uint8_t grp = 0;
                    uint8_t idx = 0;

                    if(cJSON_IsString(FunctionCode))
                    {
                        const char *str = FunctionCode->valuestring;
                        if(str[0] == 'P' || str[0] == 'p')
                        {
                            int g = 0, i = 0;
                            if(sscanf(str + 1, "%2d.%2d", &g, &i) == 2)
                            {
                                grp = (uint8_t)g;
                                idx = (uint8_t)i;
                                valid = true;
                            }
                            else if(sscanf(str + 1, "%2d%2d", &g, &i) == 2)
                            {
                                grp = (uint8_t)g;
                                idx = (uint8_t)i;
                                valid = true;
                            }
                        }
                    }
                    else if(cJSON_IsNumber(FunctionCode))
                    {
                        int num = FunctionCode->valueint;
                        grp = (uint8_t)(num / 100);
                        idx = (uint8_t)(num % 100);
                        valid = true;
                    }

                    if(valid)
                    {
                        reg_addr = ((uint16_t)grp << 8) | idx;
                    }
                }

                if(valid)
                {
                    RS485_Status_t ret = Modbus_WriteSingleReg(GD200A_SLAVE_ADDR, reg_addr, val);
                    if(ret == RS485_OK)
                    {
                        response->status = COMMAND_STATUS_OK;
                        response->payloadLength = snprintf(response->payload, sizeof(response->payload),
                            "{\"Code\":210,\"TimeStamp\":%lld,\"Message\":\"VFD WRITE REG 0x%04X VALUE %u OK\"}",
                            (long long)Sys_Info.epochtime, reg_addr, val);
                        ESP_LOGI("AZURE", "CMD 210: Write Reg 0x%04X with Value %u OK", reg_addr, val);
                    }
                    else
                    {
                        response->status = COMMAND_STATUS_BAD_REQUEST;
                        response->payloadLength = snprintf(response->payload, sizeof(response->payload),
                            "{\"Code\":210,\"TimeStamp\":%lld,\"Message\":\"VFD WRITE REG FAIL (Modbus err %d)\"}",
                            (long long)Sys_Info.epochtime, (int)ret);
                        ESP_LOGE("AZURE", "CMD 210: Write Reg FAIL, Modbus status=%d", (int)ret);
                    }
                }
                else
                {
                    response->status = COMMAND_STATUS_BAD_REQUEST;
                    response->payloadLength = snprintf(response->payload, sizeof(response->payload),
                        "{\"Code\":210,\"TimeStamp\":%lld,\"Message\":\"Missing RegAddr or Group/Index in Data\"}",
                        (long long)Sys_Info.epochtime);
                    ESP_LOGE("AZURE", "CMD 210: Missing RegAddr or Group/Index");
                }
            }
            else
            {
                response->status = COMMAND_STATUS_BAD_REQUEST;
                response->payloadLength = snprintf(response->payload, sizeof(response->payload),
                    "{\"Code\":210,\"TimeStamp\":%lld,\"Message\":\"Missing Value field in Data\"}",
                    (long long)Sys_Info.epochtime);
                ESP_LOGE("AZURE", "CMD 210: Missing Value field");
            }
        }

        /* ================================================================
         * CMD 211: AUTO RUN AFTER POWER BACK – Cấu hình tự chạy khi có điện lại
         * Payload: { "Code": 211, "TimeStamp": ..., "Data": { "Enable": 1, "DelayTime": 3.0 } }
         * ================================================================ */
        else if(_code == CMD_CODE_AUTO_RUN_AFTER_POWER_BACK) // code == 211
        {
            ESP_LOGI("AZURE", "---------- CMD VFD AUTO RUN AFTER POWER BACK (211) ----------");
            cJSON *Enable = cJSON_GetObjectItem(data, "Enable");
            cJSON *DelayTime = cJSON_GetObjectItem(data, "DelayTime");

            if(Enable != NULL && cJSON_IsNumber(Enable) && DelayTime != NULL && cJSON_IsNumber(DelayTime))
            {
                uint8_t enable_val = (uint8_t)Enable->valueint;
                float delay_val = (float)DelayTime->valuedouble;

                RS485_Status_t ret = GD200A_SetAutoRunAfterPowerBack(GD200A_SLAVE_ADDR, enable_val, delay_val);
                if(ret == RS485_OK)
                {
                    response->status = COMMAND_STATUS_OK;
                    response->payloadLength = snprintf(response->payload, sizeof(response->payload),
                        "{\"Code\":211,\"TimeStamp\":%lld,\"Message\":\"VFD AUTO RUN CONFIG OK (Enable=%d, Delay=%.1fs)\"}",
                        (long long)Sys_Info.epochtime, enable_val, delay_val);
                    ESP_LOGI("AZURE", "CMD 211: Config Auto Run OK (Enable=%d, Delay=%.1fs)", enable_val, delay_val);
                }
                else
                {
                    response->status = COMMAND_STATUS_BAD_REQUEST;
                    response->payloadLength = snprintf(response->payload, sizeof(response->payload),
                        "{\"Code\":211,\"TimeStamp\":%lld,\"Message\":\"VFD AUTO RUN CONFIG FAIL (Modbus err %d)\"}",
                        (long long)Sys_Info.epochtime, (int)ret);
                    ESP_LOGE("AZURE", "CMD 211: Config Auto Run FAIL, Modbus status=%d", (int)ret);
                }
            }
            else
            {
                response->status = COMMAND_STATUS_BAD_REQUEST;
                response->payloadLength = snprintf(response->payload, sizeof(response->payload),
                    "{\"Code\":211,\"TimeStamp\":%lld,\"Message\":\"Missing Enable or DelayTime field in Data\"}",
                    (long long)Sys_Info.epochtime);
                ESP_LOGE("AZURE", "CMD 211: Missing Enable or DelayTime fields");
            }
        }

        else
        {
            response->status = COMMAND_STATUS_BAD_REQUEST;
            response->payloadLength = sprintf(response->payload, "Unknown command code");
            ESP_LOGE("AZURE: ", "Unknown command code: %d", _code);
        }
    }    
    else
    {
        ESP_LOGE("AZURE: CMD CALLBACK", "Missing code or data feild");
        response->status = COMMAND_STATUS_BAD_REQUEST;
        response->payloadLength = sprintf(response->payload, "Missing code or data feild");
    }

    method_done:
    ESP_LOGI("AZURE: ", "---------- END CMD CALLBACK ----------\n\n\n");

}