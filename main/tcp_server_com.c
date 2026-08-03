#include "tcp_server_com.h"
#include "user_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "cJSON.h"
#include "sys/socket.h"
#include "user_ouput.h"
#include "user_crc32.h"

TaskHandle_t Tcp_Rx_Task_Handle;
TaskHandle_t Tcp_Handle_Data_Task_Handle;
TaskHandle_t Tcp_Tx_Task_Handle;

static QueueHandle_t      Tcp_Rx_Queue;
static QueueHandle_t      Tcp_Tx_Queue;

extern Sys_Info_Handle_t Sys_Info;
Tcp_Handle_t TCP_Handle;

const char *TCP_TAG = "TCP_SERVER";
const char *TCP_RX_TAG = "TCP_RX";

const char *TCP_QUEUE_TAG = "TCP_QUEUE";
const char *TCP_TX_TAG  = "TCP_TX";

const char *TCP_GET_QUEUE_TAG = "TCP GET QUEUE";
const char *TCP_RESPONSE_TAG = "TCP RESPONSE";

void Tcp_Hanle_Data_Task(void *pvParameters);
void Tcp_Tx_Task(void *pvParameters);

void TCP_Server_Communication_Init(Tcp_Handle_t *TCP_Handle)
{
    TCP_Handle->isConnected = false;
    TCP_Handle->state = MODULE_STATE_IDLE;
    // memcpy((void *)TCP_Handle->remoteIP, "192.168.0.101", strlen("192.168.0.101"));
    memcpy((void *)TCP_Handle->stationID, "TomIOTST", strlen("TomIOTST"));
    memcpy((void *)TCP_Handle->secretCode, "TomIOTSecret", strlen("TomIOTSecret"));
    strcpy(TCP_Handle->serialNumber, "MEBIONE00011225");
    // TCP_Handle->remotePort = 3000;
}

void Tcp_Server_Connect(Tcp_Handle_t *TCP_Handle)
{
    TCP_Handle->socket = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (TCP_Handle->socket < 0)
    {
        ESP_LOGE(TCP_TAG, "Unable to create socket");
        return;
    }

    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = inet_addr(TCP_Handle->remoteIP);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(TCP_Handle->remotePort);

    int err = connect(TCP_Handle->socket, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err != 0) 
    {
        ESP_LOGE(TCP_TAG, "Socket unable to connect: errno %d", errno);
        close(TCP_Handle->socket);
        vTaskDelay(5000);
        return;
    }

    ESP_LOGI(TCP_TAG, "Connect to %s:%d successfully", inet_ntoa(dest_addr.sin_addr.s_addr), ntohs(dest_addr.sin_port));

    cJSON *jsonConn = cJSON_CreateObject();
    cJSON_AddNumberToObject(jsonConn, "Code", 100);
    cJSON_AddNumberToObject(jsonConn, "TimeStamp", Sys_Info.epochtime);
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "StationId", TCP_Handle->stationID);
    cJSON_AddStringToObject(data, "Key", TCP_Handle->secretCode);
    cJSON_AddStringToObject(data, "SN", TCP_Handle->serialNumber);

    cJSON_AddItemToObject(jsonConn, "data", data);

    char *json_str = cJSON_PrintUnformatted(jsonConn);

    TCP_Handle->txLen = 0;
    memset(TCP_Handle->txbuf, 0, sizeof(TCP_Handle->txbuf));
    
    TCP_Handle->txbuf[0] = START_OF_FRAME;
    TCP_Handle->txLen++;
    memcpy(&TCP_Handle->txbuf[TCP_Handle->txLen], json_str, strlen(json_str));
    TCP_Handle->txLen += strlen(json_str);

    uint32_t crcCal = xcrc32((const unsigned char *)TCP_Handle->txbuf, TCP_Handle->txLen);

    TCP_Handle->txLen += sprintf(&TCP_Handle->txbuf[TCP_Handle->txLen], "%08x", (unsigned int)crcCal);
    TCP_Handle->txbuf[TCP_Handle->txLen] = END_OF_FRAME;
    TCP_Handle->txLen++;

    
    int sent = send(TCP_Handle->socket, TCP_Handle->txbuf, TCP_Handle->txLen, 0);
    if (sent < 0) ESP_LOGE(TCP_TAG, "Error occurred during sending: errno %d", errno);
    else ESP_LOGI(TCP_TAG, "Data connect: %.*s, %d bytes", TCP_Handle->txLen, TCP_Handle->txbuf, sent);

    cJSON_Delete(jsonConn);
    free(json_str);

    char rx_buffer[256] = {0};
    int len = recv(TCP_Handle->socket, rx_buffer, sizeof(rx_buffer) - 1, 0);

    
    if(len > 0)
    {
        rx_buffer[len] = 0;
        unsigned int crcCal = xcrc32((const unsigned char *)rx_buffer, len - 9);

        char crcStr[9] = {0};
        
        #ifdef CRC32_BYPASS
            strncpy(crcStr, rx_buffer + len -9, 8);
            crcStr[8] = '\0';
        #else
            sprintf(crcStr, "%08x", crcCal);
        #endif

        if(strncmp(crcStr, rx_buffer + len - 9, 8) == 0)
        {
            ESP_LOGI(TCP_TAG, "Data replied: %s, %d bytes", rx_buffer, len);

            cJSON *parsed = cJSON_Parse(rx_buffer);
            if (parsed) 
            {
                const cJSON *code   = cJSON_GetObjectItem(parsed, "Code");
                const cJSON *status = cJSON_GetObjectItem(parsed, "Status");

                if (cJSON_IsNumber(code) && (cJSON_IsNumber(status))) 
                {
                    if(code->valueint == 100 && status->valueint == 0)
                    {
                        TCP_Handle->isConnected = true;
                        ESP_LOGI(TCP_TAG, "Request connect to server is accepted\n");
                        TCP_Handle->state = MODULE_STATE_CONNECTED;
                    }
                    else ESP_LOGE(TCP_TAG, "Code or status invalid (code=%d, value=%d)", code->valueint, status->valueint);    
                }
                else ESP_LOGI(TCP_TAG, "Donot have 'Code' feild\n" );
                cJSON_Delete(parsed);
            }
            else ESP_LOGW(TCP_TAG, "Received data is not valid JSON");     
        }
        else ESP_LOGE("TCP_CONNECT", "Crc32 err rx (%.*s) != cal (%s)\n\n", 8, rx_buffer + len - 9, crcStr);
    }

    if(!TCP_Handle->isConnected)
    {
        ESP_LOGI(TCP_TAG, "Connect to server fail\n");
        close(TCP_Handle->socket);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    else
    {
        if(!TCP_Handle->isInitialized)
        {
            Tcp_Rx_Queue = xQueueCreate(TCP_RX_QUEUE_LENGTH, sizeof(char *));
            Tcp_Tx_Queue = xQueueCreate(TCP_TX_QUEUE_LENGTH, sizeof(char *));

            if(Tcp_Rx_Queue != NULL)
            {
                ESP_LOGI(TCP_RX_TAG, "Create rx queue successfully");
                TCP_Handle->isRxAccepted = true;
            }
            else
            {
                ESP_LOGI(TCP_RX_TAG, "Create rx queue fail\n");
                TCP_Handle->isRxAccepted = false;
            }

            if(xTaskCreatePinnedToCore(Tcp_Rx_Task, "TCP Rx Task", 3*4096, NULL, 2, &Tcp_Rx_Task_Handle, 0) == pdPASS)
            {
                ESP_LOGI(TCP_TAG, "Create rx thread suscessfully");
                TCP_Handle->state = MODULE_STATE_EXCHANGE_DATA;
            }
            else ESP_LOGI(TCP_TAG, "Create rx thread fail\n");
            if(xTaskCreatePinnedToCore(Tcp_Hanle_Data_Task, "TCP Handle Data Task", 3*4096, NULL, 1, &Tcp_Handle_Data_Task_Handle, 1) == pdPASS)
            {
                ESP_LOGI(TCP_TAG, "Create handle thread suscessfully");
                TCP_Handle->state = MODULE_STATE_EXCHANGE_DATA;
            }
            else ESP_LOGI(TCP_TAG, "Create handle thread fail\n");
            if(xTaskCreatePinnedToCore(Tcp_Tx_Task, "TCP Tx Task", 3*4096, NULL, 1, &Tcp_Tx_Task_Handle, 0) == pdPASS)
            {
                ESP_LOGI(TCP_TAG, "Create tx thread suscessfully");
                TCP_Handle->state = MODULE_STATE_EXCHANGE_DATA;
            }
            else ESP_LOGI(TCP_TAG, "Create tx thread fail\n");
            TCP_Handle->isInitialized = true;
        }
        else ESP_LOGI(TCP_TAG, "Reconnect ... done\n");
    }
}

void Tcp_Tx_Task(void *pvParameters)
{
    char *pData;
    while(1)
    {
        if(xQueueReceive(Tcp_Tx_Queue, &pData, pdMS_TO_TICKS(100)) == pdPASS)
        {
            if(pData != NULL)
            {
                ESP_LOGI(TCP_TX_TAG, "Msg: %s\n", pData);
                Tcp_Transmit_Process(&TCP_Handle, pData);
                free(pData);
            }
            else ESP_LOGE(TCP_TX_TAG, "Data NULL\n");
            
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void Tcp_Hanle_Data_Task(void *pvParameters)
{
    char *pData;
    while(1)
    {
        if(xQueueReceive(Tcp_Rx_Queue, &pData, pdMS_TO_TICKS(100)) == pdPASS)
        {
            if(pData != NULL)
            {
                ESP_LOGI(TCP_GET_QUEUE_TAG, "Msg: %s", pData);
                Tcp_Handle_Process(&TCP_Handle, pData);
                free(pData);
            }
            else ESP_LOGE(TCP_GET_QUEUE_TAG, "Data NULL\n");
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void Tcp_Rx_Task(void *pvParameters)
{
    while(1)
    {
        if(TCP_Handle.isRxAccepted && TCP_Handle.isConnected)
        {
            int bytes = recv(TCP_Handle.socket, (void *)TCP_Handle.rxbuf, sizeof(TCP_Handle.rxbuf) - 1, 0);
            if(bytes > 0)
            {
                TCP_Handle.rxbuf[bytes] = '\0';
                ESP_LOGI(TCP_RX_TAG, "%s", TCP_Handle.rxbuf);

                unsigned int crcCal = xcrc32((const unsigned char *)TCP_Handle.rxbuf, bytes - 9);

                char crcStr[9] = {0};

                #ifdef CRC32_BYPASS
                    strncpy(crcStr, TCP_Handle.rxbuf + bytes - 9, 8);
                    crcStr[8] = '\0';
                #else
                    sprintf(crcStr, "%08x", crcCal);
                #endif
                

                if(strncmp(crcStr, TCP_Handle.rxbuf + bytes - 9, 8) == 0)
                {
                    cJSON *parsed = cJSON_Parse(TCP_Handle.rxbuf);

                    if (parsed)
                    {
                        const cJSON *code   = cJSON_GetObjectItem(parsed, "Code");

                        if(cJSON_IsNumber(code))
                        {
                            char *pMsg = strdup((const char *)TCP_Handle.rxbuf);
                            if(pMsg != NULL)
                            {
                                if(xQueueSend(Tcp_Rx_Queue, &pMsg, pdMS_TO_TICKS(50)) != pdPASS)
                                {
                                    ESP_LOGE(TCP_RX_TAG, "Cannot send to queue\n");
                                    free(pMsg);
                                }
                            }
                            else ESP_LOGE(TCP_RX_TAG,"Cannot allocate buffer data\n");  
                        }
                        else ESP_LOGI(TCP_RX_TAG, "Format invalid\n");                             
                        cJSON_Delete(parsed);
                    }
                    else ESP_LOGW(TCP_RX_TAG, "Received data is not valid JSON");                   
                }
                else ESP_LOGE(TCP_RX_TAG, "Crc32 err: rx (%.*s) != cal (%s)\n\n\n", 8, TCP_Handle.rxbuf + bytes - 9, crcStr);
            }
            else if(bytes == 0)
            {
                ESP_LOGW(TCP_RX_TAG, "Connection is closed by remote host, close recent socket and try reconnect");
                close(TCP_Handle.socket);
                TCP_Handle.socket = -1;
                TCP_Handle.state = MODULE_STATE_IDLE;
                TCP_Handle.isConnected = false;
            }
            else ESP_LOGI(TCP_RX_TAG, "Connection close or err\n\n\n");  
        }        
        vTaskDelay(pdTICKS_TO_MS(10));
    }
}

// void Tcp_Server_Exchange_Data(Tcp_Handle_t *TCP_Handle)
// {

    
// }

void Tcp_Server_Communication_Task(Tcp_Handle_t *TCP_Handle)
{
    TCP_Server_Communication_Init(TCP_Handle);

    while(!Sys_Info.isWifiConnected)
    {
        ESP_LOGI(TCP_TAG, "Wait for wifi connected...\n");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    while(1)
    {
        switch (TCP_Handle->state)
        {
            case MODULE_STATE_IDLE:
                /* code */
                Tcp_Server_Connect(TCP_Handle);
                break;
            
            case MODULE_STATE_CONNECTED:
                // Tcp_Server_Exchange_Data(TCP_Handle);
                break;
            
            case MODULE_STATE_EXCHANGE_DATA:
                break;
            
            default:
                break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void Tcp_Transmit_Process(Tcp_Handle_t *TCP_Handle, const char *pData)
{

}

void Tcp_Handle_Process(Tcp_Handle_t *TCP_Handle, const char *pData)
{
    cJSON *parsed = cJSON_Parse(pData);
    if (parsed) 
    {
        const cJSON *code   = cJSON_GetObjectItem(parsed, "Code");
        TCP_Handle->code = code->valueint;
        
        if(TCP_Handle->code == CODE_TURN_ON_OFF)
        {
            // const cJSON *output = cJSON_GetObjectItem(parsed, "Output");
        
            const cJSON *data = cJSON_GetObjectItemCaseSensitive(parsed, "Data");
            if(data)
            {
                const cJSON *deviceName = cJSON_GetObjectItem(data, "DeviceName");
                const cJSON *deviceId = cJSON_GetObjectItem(data, "DeviceId");
                const cJSON *value = cJSON_GetObjectItem(data, "Value");

                if(cJSON_IsNumber(value))
                {
                    if(cJSON_IsNumber(deviceId))
                    {
                        uint8_t hiNibble = (deviceId->valueint / 10);
                        uint8_t loNiblle = (deviceId->valueint % 10);
                        uint8_t shift = (hiNibble*4) - (4-loNiblle) - 1;

                        // ESP_LOGW("TCP_HANDLE: ", "hi:%d, low:%d, shift:%d", hiNibble, loNiblle, shift);

                        if(value->valueint == 0) DeviceHandle.outputBuf &= ~(1 << shift);                        
                        else DeviceHandle.outputBuf |= (1 << shift);                        
                        sprintf(TCP_Handle->txbuf, "DeviceName: %s, DeviceId %d, Turn %s", deviceName->valuestring, deviceId->valueint, (value->valueint == 1) ? "ON" : "OFF");
                        xEventGroupSetBits(IO_Event_Group, SERVER_TRIGGER_IO_BIT);
                    }
                    else
                    {
                        strcpy(TCP_Handle->txbuf, "Output must be number or array");
                        ESP_LOGE("TCP_HANDLE: ", "Output must be number or array");
                    }


                }
                else
                {
                    strcpy(TCP_Handle->txbuf, "Value must be a number");
                    ESP_LOGE("TCP_HANDLE: ", "Value must be a number");
                }

            }
            else ESP_LOGE("TCP_HANDLE", "Format error, need \"Data\" feild");                        
        }
        else if(TCP_Handle->code == CODE_SYS_RESET)
        {
            cJSON *data = cJSON_GetObjectItem(parsed, "Data");
            if(cJSON_IsObject(data))
            {
                const cJSON *subCode = cJSON_GetObjectItem(data, "SubCode");
                if(cJSON_IsNumber(subCode))
                {
                    if(subCode->valueint == SUBCODE_SYS_RESET)
                    {
                        ESP_LOGW("TCP_HANDLE: ", "Reset system now");
                        strcpy(TCP_Handle->txbuf, "Reset system now");
                        TCP_Handle->isSysRestart = true;
                    }
                    else
                    {
                        strcpy(TCP_Handle->txbuf, "SubCode incorrect");
                        ESP_LOGE("TCP_HANDLE: ", "SubCode incorrect");
                    }
                }
                else
                {
                    strcpy(TCP_Handle->txbuf, "SubCode must be a number");
                    ESP_LOGE("TCP_HANDLE: ", "SubCode must be a number");
                }
            }
            else
            {
                strcpy(TCP_Handle->txbuf, "Missing 'Data' object");
                ESP_LOGE("TCP_HANDLE: ", "Missing 'Data' object");
            }
        }
        else
        {
            strcpy(TCP_Handle->txbuf, "Unknow cmd");
            ESP_LOGE("TCP_HANDLE: ", "Unknow cmd");
        }
        cJSON_Delete(parsed);
    }
    else
    {
        strcpy(TCP_Handle->txbuf, "Json format invalid");
        ESP_LOGW(TCP_TAG, "Json format invalid");
    }

    if(Tcp_Response_To_Server(TCP_Handle, CODE_RESPONSE_TO_SERVER) == false)
    {
        ESP_LOGI("TCP_RESPONSE: ", "Response to server fail");
    }

    if(TCP_Handle->isSysRestart)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }

}

bool Tcp_Response_To_Server(Tcp_Handle_t *TCP_Handle, uint16_t code)
{
    cJSON *jsonRes = cJSON_CreateObject();
    if(jsonRes == NULL)
    {
        ESP_LOGE("TCP PREPARE: ", "Do not enough memory");
        return false;
    }

    uint16_t _code = 0;
    if(code == CODE_RESPONSE_TO_SERVER) _code = TCP_Handle->code; 
    else _code = code;
    
    cJSON_AddNumberToObject(jsonRes, "Code", _code);
    cJSON_AddNumberToObject(jsonRes, "TimeStamp", TCP_Handle->epochtime);
    cJSON_AddStringToObject(jsonRes, "Message", TCP_Handle->txbuf);

    char *jsonStr = cJSON_PrintUnformatted(jsonRes);

    if(jsonStr == NULL)
    {
        ESP_LOGE("TCP PREPARE: ", "Do not enough memory");
        cJSON_Delete(jsonRes);
        return false;
    }

    memset(TCP_Handle->txbuf, 0, sizeof(TCP_Handle->txbuf));
    TCP_Handle->txLen = 0;

    TCP_Handle->txbuf[0] = START_OF_FRAME;
    TCP_Handle->txLen++;

    memcpy(&TCP_Handle->txbuf[TCP_Handle->txLen], jsonStr, strlen(jsonStr));
    TCP_Handle->txLen += strlen(jsonStr);

    unsigned int crcCal = xcrc32((const unsigned char *)TCP_Handle->txbuf, TCP_Handle->txLen);
    TCP_Handle->txLen += sprintf(&TCP_Handle->txbuf[TCP_Handle->txLen], "%08x", (unsigned int)crcCal);


    TCP_Handle->txbuf[TCP_Handle->txLen] = END_OF_FRAME;
    TCP_Handle->txLen++;

    cJSON_Delete(jsonRes);
    free(jsonStr);

    int sent = send(TCP_Handle->socket, TCP_Handle->txbuf, TCP_Handle->txLen, 0);
    if (sent < 0)
    {
        ESP_LOGE(TCP_TAG, "Error occurred during sending: errno %d", errno);
        return false;
    }

    TCP_Handle->txbuf[TCP_Handle->txLen] = '\0';
    ESP_LOGI(TCP_RESPONSE_TAG, "%s, successfully\n\n\n", TCP_Handle->txbuf);

    return true;
}