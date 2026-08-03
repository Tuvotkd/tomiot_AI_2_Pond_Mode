#pragma once

#include <stdint.h>
#include <string.h>
#include "stdbool.h"
#include "time.h"

#define CODE_CONNECTION                 100
#define CODE_TURN_ON_OFF                101
#define CODE_SYS_RESET                  199
#define CODE_RESPONSE_TO_SERVER         999

#define SUBCODE_SYS_RESET   9000

#define TCP_RX_QUEUE_LENGTH     10
#define TCP_TX_QUEUE_LENGTH     10

typedef enum 
{
    MODULE_STATE_IDLE = 0,
    MODULE_STATE_CONNECTED,
    MODULE_STATE_EXCHANGE_DATA,

}connect_state_t;

typedef struct 
{
    /* data */
    bool isConnected;
    bool isRxAccepted;
    bool isInitialized;
    uint8_t state;
    uint8_t code;
    char remoteIP[20];
    uint16_t remotePort;

    char stationID[64];
    char secretCode[64];
    char serialNumber[16];

    char ssid[64];
    char pass[64];

    char rxbuf[256];
    char txbuf[256];

    uint16_t txLen;
    int socket;
    bool isSysRestart;

    time_t epochtime;
}Tcp_Handle_t;

extern Tcp_Handle_t TCP_Handle;

void Tcp_Rx_Task(void *pvParameters);

void TCP_Server_Communication_Init(Tcp_Handle_t *TCP_Handle);

void Tcp_Server_Connect(Tcp_Handle_t *TCP_Handle);

void Tcp_Server_Communication_Task(Tcp_Handle_t *TCP_Handle);

void Tcp_Transmit_Process(Tcp_Handle_t *TCP_Handle, const char *pData);

void Tcp_Handle_Process(Tcp_Handle_t *TCP_Handle, const char *pData);

bool Tcp_Response_To_Server(Tcp_Handle_t *TCP_Handle, uint16_t code);