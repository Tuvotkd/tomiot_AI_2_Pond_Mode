#include "user_system.h"
#include "tcp_server_com.h"
#include "user_storage.h"
#include "esp_log.h"
#include "user_azure.h"
#include "esp_system.h"
#include "wifi_config_manager.h"
#include "user_fram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

Sys_Info_Handle_t Sys_Info;
uint32_t reset_count = 0;

static void User_System_Update_Reset_Count(void)
{
    uint32_t count = 0;
    if(!Nvs_Read_Number(STORAGE_KEY_RESET_COUNT, &count))
    {
        count = 0;
    }

    esp_reset_reason_t reason = esp_reset_reason();
    if(!(reason == ESP_RST_POWERON && count == 0))
    {
        count++;
        Nvs_Write_Number(STORAGE_KEY_RESET_COUNT, count);
    }

    reset_count = count;
    ESP_LOGI("SYS INIT", "Reset reason=%d, reset_count=%u", (int)reason, (unsigned)reset_count);
}

void User_System_Clear_Reset_Count(void)
{
    reset_count = 0;
    Nvs_Write_Number(STORAGE_KEY_RESET_COUNT, 0);
    ESP_LOGI("SYS INIT", "Reset count cleared");
}

void User_System_Get_Config(void)
{
    memset(TCP_Handle.remoteIP, 0, sizeof(TCP_Handle.remoteIP));
    memset(TCP_Handle.ssid, 0, sizeof(TCP_Handle.ssid));
    memset(TCP_Handle.pass, 0, sizeof(TCP_Handle.pass));

    memset(&IoTHubHandle, 0, sizeof(IoTHubHandle));

    ESP_LOGI("SYS INIT", "Get Tcp parameters");

    // Keep STA credentials empty at boot; wifi_config_manager will load from FRAM when available.

    /* Nạp cấu hình Azure từ FRAM vào RAM thông qua hàm dùng chung */
    User_Azure_LoadConfig();

    /* Nạp cấu hình Feeder Mode, Oxy Mode và Pond Mode từ FRAM */
    if (!Fram_Init())
    {
        ESP_LOGE("SYS INIT", "FRAM init failed for Feeder/Oxy Mode config. Using defaults.");
        Sys_Info.feederMode = 0;
        Sys_Info.activeFeederId = 99;
        Sys_Info.oxyMode = 0;
        Sys_Info.activeOxyId = 99;
        Sys_Info.pondMode = POND_MODE_10_DEV;
        Sys_Info.vfdEnabled = 1; // default to enabled
    }
    else
    {
        // Đọc feederMode (1 byte)
        if (!Fram_Read_Data(FRAM_FEEDER_MODE_ADDR, &Sys_Info.feederMode, 1))
        {
            Sys_Info.feederMode = 0; // Mặc định chế độ cũ
        }
        // Đọc activeFeederId (2 bytes)
        uint8_t id_buf[2] = {0};
        if (Fram_Read_Data(FRAM_FEEDER_ACTIVE_ID_ADDR, id_buf, 2))
        {
            Sys_Info.activeFeederId = (id_buf[0] << 8) | id_buf[1];
        }
        else
        {
            Sys_Info.activeFeederId = (Sys_Info.feederMode == 1) ? 32 : 99;
        }
        // Kiểm tra tính hợp lệ
        if (Sys_Info.feederMode > 1)
        {
            Sys_Info.feederMode = 0;
        }
        if (Sys_Info.feederMode == 0)
        {
            Sys_Info.activeFeederId = 99;
        }
        else if (Sys_Info.activeFeederId != 31 && Sys_Info.activeFeederId != 32)
        {
            Sys_Info.activeFeederId = 32; // Mặc định M2 hoạt động ở Mode 1
        }

        // 2. Đọc Oxy Mode
        if (!Fram_Read_Data(FRAM_OXY_MODE_ADDR, &Sys_Info.oxyMode, 1))
        {
            Sys_Info.oxyMode = 0;
        }
        uint8_t id_oxy_buf[2] = {0};
        if (Fram_Read_Data(FRAM_OXY_ACTIVE_ID_ADDR, id_oxy_buf, 2))
        {
            Sys_Info.activeOxyId = (id_oxy_buf[0] << 8) | id_oxy_buf[1];
        }
        else
        {
            Sys_Info.activeOxyId = (Sys_Info.oxyMode == 1) ? 22 : 99;
        }
        if (Sys_Info.oxyMode > 1) Sys_Info.oxyMode = 0;
        if (Sys_Info.oxyMode == 0)
        {
            Sys_Info.activeOxyId = 99;
        }
        else if (Sys_Info.activeOxyId != 21 && Sys_Info.activeOxyId != 22)
        {
            Sys_Info.activeOxyId = 22;
        }

        ESP_LOGI("SYS INIT", "Feeder Config Loaded: Mode=%d, ActiveID=%d", Sys_Info.feederMode, Sys_Info.activeFeederId);
        ESP_LOGI("SYS INIT", "Oxy Config Loaded: Mode=%d, ActiveID=%d", Sys_Info.oxyMode, Sys_Info.activeOxyId);

        // 3. Đọc Pond Mode (1 byte)
        uint8_t pond_mode_raw = 0;
        if (!Fram_Read_Data(FRAM_POND_MODE_ADDR, &pond_mode_raw, 1))
        {
            pond_mode_raw = POND_MODE_10_DEV;
        }
        if (pond_mode_raw != POND_MODE_4_DEV) pond_mode_raw = POND_MODE_10_DEV; // chỉ chấp nhận 0 hoặc 1
        Sys_Info.pondMode = pond_mode_raw;
        ESP_LOGI("SYS INIT", "Pond Mode Loaded: %s", Sys_Info.pondMode == POND_MODE_4_DEV ? "Mode 4 thiết bị" : "Mode 10 thiết bị");

        // 4. Đọc VFD Enabled (1 byte)
        uint8_t vfd_enabled_raw = 1;
        if (!Fram_Read_Data(FRAM_VFD_ENABLED_ADDR, &vfd_enabled_raw, 1))
        {
            vfd_enabled_raw = 1;
        }
        if (vfd_enabled_raw > 1) vfd_enabled_raw = 1;
        Sys_Info.vfdEnabled = vfd_enabled_raw;
        ESP_LOGI("SYS INIT", "VFD Enabled Loaded: %s", Sys_Info.vfdEnabled == 1 ? "Kích hoạt (Enable)" : "Vô hiệu hóa (Disable)");

        // 5. Đọc cấu hình Performance Monitor (1 byte)
        uint8_t perf_enabled_raw = 0;
        if (!Fram_Read_Data(FRAM_PERF_MONITOR_ENABLED_ADDR, &perf_enabled_raw, 1))
        {
            perf_enabled_raw = 0; // Mặc định là tắt
        }
        if (perf_enabled_raw > 1) perf_enabled_raw = 0;
        Sys_Info.perfMonitorEnabled = perf_enabled_raw;
        ESP_LOGI("SYS INIT", "Perf Monitor Enabled Loaded: %s", Sys_Info.perfMonitorEnabled == 1 ? "Bật (Enable)" : "Tắt (Disable)");

        // 6. Đọc cấu hình Console Monitor (1 byte)
        uint8_t console_enabled_raw = 0;
        if (!Fram_Read_Data(FRAM_CONSOLE_MONITOR_ENABLED_ADDR, &console_enabled_raw, 1))
        {
            console_enabled_raw = 0; // Mặc định là tắt
        }
        if (console_enabled_raw > 1) console_enabled_raw = 0;
        Sys_Info.consoleMonitorEnabled = console_enabled_raw;
        ESP_LOGI("SYS INIT", "Console Monitor Enabled Loaded: %s", Sys_Info.consoleMonitorEnabled == 1 ? "Bật (Enable)" : "Tắt (Disable)");
    }
}

bool Is_System_Time_Synchronized(void)
{
    return Sys_Info.isTimeSync;
}

bool Is_System_Internet_Connected(void)
{
    return Sys_Info.isWifiConnected;
}

void User_System_Init(void)
{
    /* Get config parameters */
    ESP_LOGI("SYS INIT", "");

    User_System_Update_Reset_Count();
    User_System_Get_Config();
}

void User_System_Search_Device(void)
{
    Sys_Info.searchDeviceTimestamp = xTaskGetTickCount();
}
