#pragma once

#include "stdbool.h"
#include "stdint.h"

#define CONSOLE_RX_BUF_LEN 1024

typedef struct 
{
    bool isReceived;
    char rxBuf[CONSOLE_RX_BUF_LEN];
    uint16_t rxLen;
}Console_Handle_t;


extern Console_Handle_t consoleHandle;


void User_Console_Handle(char *pData);

void User_Console_Task(void);

