#pragma once

#include "esp_https_ota.h"
#include "freertos/FreeRTOS.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#define OTA_WAIT_BIT   (1<<0)


extern EventGroupHandle_t otaEventGroup;
extern char g_ota_update_url[256];

void User_Ota_Task(void);
esp_err_t update_firmware(const char *updateFileName);
