#pragma once

#include "stdbool.h"
#include "stdint.h"

#define STORAGE_KEY_SEVER_IP          "server_ip"
#define STORAGE_KEY_SEVER_PORT        "server_port"
#define STORAGE_KEY_WIFI_SSID         "wifi_ssid"
#define STORAGE_KEY_WIFI_PASS         "wifi_pass"


/* Define for Azure IoT Hub */
#define STORAGE_KEY_IOT_HUB_HOST_NAME       "iot_hub_host"
#define STORAGE_KEY_IOT_HUB_DEVICE_ID       "iot_device_id"
#define STORAGE_KEY_IOT_HUB_SYMMETRIC_KEY   "iot_symm_key"
#define STORAGE_KEY_RESET_COUNT             "reset_count"





bool Nvs_Write_String(const char *key, const char *value);

bool Nvs_Write_Number(const char *key, uint32_t value);

bool Nvs_Read_String(const char *key, char *value);

bool Nvs_Read_Number(const char *key, uint32_t *value);

void Nvs_Storage_Task(void);
