#pragma once


void User_Azure_Task(void);
void User_Azure_LoadConfig(void);

#include "stdint.h"
#include "stdbool.h"
#include "azure_iot_hub_client.h"
#include "cJSON.h"

#define IOT_HUB_HOST_NAME_LEN       64
#define IOT_HUB_DEVICE_ID_LEN       32
#define IOT_HUB_SYMMETRIC_KEY_LEN   64


typedef struct
{
    char hostName[IOT_HUB_HOST_NAME_LEN];
    char deviceId[IOT_HUB_DEVICE_ID_LEN];
    char symmetricKey[IOT_HUB_SYMMETRIC_KEY_LEN];

    bool isNeedReinit;
    bool isAzureInitialized;
    bool isProcessLoopInitialized;
    bool isTransmitInitialized;

}IoTHubHandle_t;

extern IoTHubHandle_t IoTHubHandle;



typedef struct
{
    char *payload;
}TelemetryEvent_t;

// typedef struct
// {
//     uint32_t DeviceId;
//     char DeviceName[16];
// } device_t;


typedef enum
{
    COMMAND_STATUS_OK = 200,
    COMMAND_STATUS_BAD_REQUEST = 400,
    COMMAND_STATUS_NOT_FOUND = 404,
    COMMAND_STATUS_TOO_MANY_REQUEST = 429,
    COMMAND_STATUS_DEVICE_ERROR = 500
}CommandStatus_t;

typedef struct
{
    CommandStatus_t status;
    char payload[256];
    uint16_t payloadLength;
} DirectMethodResponse_t;



void User_Azure_Task(void);

BaseType_t PushTelemetry(const char *payload);

void Azure_Handle_Direct_Method_Data(cJSON *payload, DirectMethodResponse_t *response);
bool FRAM_SaveDevice(uint8_t index);
void FRAM_LoadDevice(uint8_t index);
bool Device_Add(uint32_t id, const char *name);
uint16_t User_FRAM_Get_Device_Address(uint8_t index);
bool User_FRAM_Get_Slot_By_Device_Index(uint8_t index, uint8_t *slot_out);
