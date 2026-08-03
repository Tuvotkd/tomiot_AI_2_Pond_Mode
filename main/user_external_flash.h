#pragma once

#include "esp_err.h"
#include "stdbool.h"
#include "esp_flash.h"

#define PIN_NUM_FLASH_CS 10

/**
 * @brief Initialize the external SPI Flash chip W25Q128
 * @return esp_err_t ESP_OK on success
 */
esp_err_t User_External_Flash_Init(void);

/**
 * @brief Mount the SPIFFS filesystem on the external Flash partition
 * @return esp_err_t ESP_OK on success
 */
esp_err_t User_External_Flash_Mount(void);

/**
 * @brief Format the external Flash partition
 * @return esp_err_t ESP_OK on success
 */
esp_err_t User_External_Flash_Format(void);

/**
 * @brief Check if a file exists in the filesystem
 * @param path The filepath (e.g. "/web/index.html")
 * @return true if exists
 */
bool User_External_Flash_File_Exists(const char *path);

/**
 * @brief Push an Azure command message payload into the log queue for writing to Flash
 * @param raw_json_payload The raw payload string received from Azure
 * @return esp_err_t ESP_OK on success
 */
esp_err_t User_External_Flash_Log_Azure_Command(const char *raw_json_payload);

/**
 * @brief Log Wi-Fi or Azure disconnect/reconnect events to /web/Wifi_Azure_History.csv
 * @param event_type Event name string (e.g., "WIFI_DISCONNECTED", "AZURE_CONNECTED")
 * @param details Additional detail string (e.g., RSSI, IP, Reason Code)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t User_External_Flash_Log_Event(const char *event_type, const char *details);

/**
 * @brief FreeRTOS task that writes Azure logs to Flash and manages 7-day log rotation
 */
void User_External_Flash_Task(void *pvParameters);
