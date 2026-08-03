#pragma once

#include <stdbool.h>
#include <stddef.h>

#define WIFI_FRAM_ADDR  0x1400
#define WIFI_FRAM_SIZE  128

#define AZURE_FRAM_ADDR 0x1480
#define AZURE_FRAM_SIZE 256


#define WIFI_AP_SSID    "MEBIECO_CONFIG"
#define WIFI_AP_PASS    "Mebieco@69696969"
#define WIFI_AP_CHANNEL 1
#define WIFI_AP_MAX_CONN 4

bool wifi_config_manager_init(void);
bool wifi_config_manager_load(char *ssid_out, size_t ssid_len, char *pass_out, size_t pass_len);
bool wifi_config_manager_save(const char *ssid, const char *pass);
bool wifi_config_manager_clear(void);
bool azure_config_manager_load(char *host, size_t host_len, char *dev, size_t dev_len, char *sym, size_t sym_len);
bool azure_config_manager_save(const char *host, const char *dev, const char *sym);
bool azure_config_manager_clear(void);
void wifi_config_manager_schedule_connect(void);
void wifi_config_manager_prepare_scan(void);
