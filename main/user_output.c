#include "user_ouput.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "stdint.h"
#include "string.h"
#include "driver/i2c_master.h"
#include "user_system.h"
#include "cJSON.h"
#include "user_crc32.h"
#include "user_fram.h"
#include "user_azure.h"
#include "time.h"
#include "user_fram.h"
#include "esp_timer.h"
#include "RS485.h"
#include "esp_task_wdt.h"
#include <math.h>


void User_Input_Task(void);



#define IO_CLK_HIGH()       gpio_set_level(IO_CLK_PIN, 1)
#define IO_CLK_LOW()        gpio_set_level(IO_CLK_PIN, 0)

#define IO_STR_HIGH()       gpio_set_level(IO_STR_PIN, 1)
#define IO_STR_LOW()        gpio_set_level(IO_STR_PIN, 0)

#define  IO_CLK_PIN         16
#define  IO_MOSI_PIN        17
#define  IO_STR_PIN         18
#define  OUTPUT_ENABLE_PIN  15

#define DEVICE_SIZE           sizeof(Device_t)

const char *IO_TAG = "IO OUTPUT: ";
const char *IO_INPUT_TAG = "IO_INPUT";

i2c_master_bus_handle_t i2cExHandle;
i2c_master_dev_handle_t i2cExDevHandle;

Device_Handle_t DeviceHandle;
IO_Input_Config_Handle_t InputConfHandle;
EventGroupHandle_t IO_Event_Group;
static uint32_t outputBuf;
static uint32_t device_fault_states = 0;

bool isManualFeeder_M2 = false;
uint32_t zeroScale = 389860;

double vfd_last_energy = 0.0;
double vfd_daily_energy = 0.0;

uint32_t zero500 = 488850;

uint8_t calibFactor = 200;

/* Input function Prototypes */
static esp_err_t User_Device_Get_Error_Status(uint8_t *data, uint16_t size);

static bool isInputInit = false;

void User_Device_Params_Init(Device_Handle_t *handle, int i)
{
    handle->Device[i].name[sizeof(handle->Device[i].name) - 1] = '\0';
}

/**
 * @brief Khôi phục toàn bộ cấu hình thiết bị và lịch trình từ FRAM vào RAM
 * Chỉ chạy 1 lần duy nhất khi khởi động hệ thống
 */
void User_Device_Config_Sync_With_Fram(Device_Handle_t *handle)
{
    bool fram_ok = Fram_Init();
    if(!fram_ok)
    {
        ESP_LOGE("FRAM_LOAD", "FRAM init failed, Using default RAM values");
        return;
    }

    ESP_LOGI("FRAM_LOAD", "Starting sync RAM with FRAM... PondMode=%d", Sys_Info.pondMode);

    for(int i = 0; i < DEVICE_MAX_NUM; i++)
    {
        // Lưu lại các thông tin cấu hình cứng từ code (nếu FRAM chưa có thì dùng cái này)
        uint16_t configured_id = handle->Device[i].id;
        bool configured_active = handle->Device[i].isActived;
        char configured_name[32] = {0};
        strncpy(configured_name, handle->Device[i].name, 31);

        // ─── Áp dụng quy tắc lọc theo Pond Mode ───
        // Trong Mode 4 thiết bị, chỉ kích hoạt ID: 11, 12, 21, 41
        // Các thiết bị khác (13, 14, 22, 31, 32, 42) bị vô hiệu hóa
        if (Sys_Info.pondMode == POND_MODE_4_DEV)
        {
            uint16_t dev_id = configured_id;
            if (dev_id == 11 || dev_id == 12 || dev_id == 21 || dev_id == 41)
            {
                configured_active = true;
            }
            else
            {
                configured_active = false;
            }
        }

        // Nạp từ FRAM vào RAM
        FRAM_LoadDevice((uint8_t)i);

        // Nếu FRAM trống (id=0) hoặc lỗi, trả lại các thông tin định danh cơ bản
        if(handle->Device[i].id == 0)
        {
            handle->Device[i].id = configured_id;
            strncpy(handle->Device[i].name, configured_name, 31);
        }
        
        handle->Device[i].isActived = configured_active; // Luôn ưu tiên trạng thái Active theo mode ao
        
        // Kiểm tra tính hợp lệ của dữ liệu nạp từ FRAM
        if(handle->Device[i].scheduleCount > DEVICE_SCHEDULE_MAX)
        {
            handle->Device[i].scheduleCount = 0;
            handle->Device[i].isScheduled = false;
        }

        // Tự động sửa lỗi nếu scheduleIndex vượt quá số lượng lịch
        if(handle->Device[i].scheduleIndex >= handle->Device[i].scheduleCount && handle->Device[i].scheduleCount > 0)
        {
            handle->Device[i].scheduleIndex = 0;
        }

        // Đồng bộ thời gian hiện hành (startTime/stopTime) từ lịch trình hiện tại của device
        if(handle->Device[i].isScheduled && handle->Device[i].scheduleCount > 0)
        {
            uint8_t idx = handle->Device[i].scheduleIndex;
            handle->Device[i].startTime = handle->Device[i].schedules[idx].startTime;
            handle->Device[i].stopTime  = handle->Device[i].schedules[idx].stopTime;
        }

        // Nếu thiết bị bị vô hiệu hóa trong Mode 4, đảm bảo relay OFF ngay
        if (!handle->Device[i].isActived)
        {
            handle->Device[i].state = DEVICE_STATE_OFF;
            handle->Device[i].isScheduled = false;
            handle->Device[i].scheduleCount = 0;
        }

        ESP_LOGI("FRAM_LOAD", "Synced Device[%d]: ID=%d, Name=%s, Active=%s, Scheduled=%s, Count=%d", 
                 i, handle->Device[i].id, handle->Device[i].name,
                 handle->Device[i].isActived ? "YES" : "NO",
                 handle->Device[i].isScheduled ? "YES" : "NO",
                 handle->Device[i].scheduleCount);
    }

    // Trong Mode 4 thiết bị: đảm bảo activeOxyId chỉ trỏ đến AirBlower 1 (ID: 21)
    if (Sys_Info.pondMode == POND_MODE_4_DEV)
    {
        Sys_Info.activeOxyId = 21;
        ESP_LOGI("FRAM_LOAD", "Pond Mode 4: activeOxyId forced to 21");
    }

    ESP_LOGI("FRAM_LOAD", "Sync completed. Persistence sizes: Persist=%u, Schedule=%u", 
             (unsigned)sizeof(Device_Persist_t), (unsigned)sizeof(Device_Schedule_t));
}

/**
 * @brief Chuyển đổi chế độ ao nuôi (Pond Mode), thực hiện:
 *        1. Xóa N slot FRAM thiết bị tương ứng với chế độ cũ
 *        2. Ghi pondMode mới vào FRAM (0x1610)
 *        3. Reboot ESP32 để tải lại toàn bộ cấu hình sạch
 * @param new_mode POND_MODE_10_DEV (0) hoặc POND_MODE_4_DEV (1)
 */
void User_Device_Switch_Pond_Mode(uint8_t new_mode)
{
    // Validate
    if (new_mode != POND_MODE_10_DEV && new_mode != POND_MODE_4_DEV)
    {
        ESP_LOGE("POND_MODE", "Invalid new_mode=%d", new_mode);
        return;
    }

    if (new_mode == Sys_Info.pondMode)
    {
        ESP_LOGW("POND_MODE", "Already in mode %d, skipping switch", new_mode);
        return;
    }

    ESP_LOGW("POND_MODE", "Switching Pond Mode: %d -> %d",
             Sys_Info.pondMode, new_mode);

    // ─── Bước 0: Tắt lập tức toàn bộ ngõ ra phần cứng và đưa trạng thái RAM về OFF ───
    DeviceHandle.outputBuf = 0;
    User_Out_Put_Flush_All(0);

    for (int i = 0; i < DEVICE_MAX_NUM; i++)
    {
        DeviceHandle.Device[i].state = DEVICE_STATE_OFF;
        DeviceHandle.Device[i].isScheduled = false;
        DeviceHandle.Device[i].isSchedulePaused = false;
        DeviceHandle.Device[i].scheduleCount = 0;
        DeviceHandle.Device[i].scheduleIndex = 0;
        DeviceHandle.Device[i].startTime = 0;
        DeviceHandle.Device[i].stopTime = 0;
    }
    ESP_LOGI("POND_MODE", "All hardware outputs turned OFF and RAM states cleared to DEVICE_STATE_OFF");

    // ─── Bước 1: Xóa toàn bộ 10 slot FRAM thiết bị về 0 ───
    uint8_t slots_to_clear = 10;
    ESP_LOGI("POND_MODE", "Clearing %d device slots in FRAM...", slots_to_clear);
    FRAM_Clear_Device_Slots(slots_to_clear);

    // ─── Bước 2: Ghi chế độ mới vào FRAM ───
    uint8_t mode_val = new_mode;
    Fram_Write_Data(FRAM_POND_MODE_ADDR, &mode_val, 1);
    ESP_LOGI("POND_MODE", "Pond Mode %d written to FRAM at 0x%04X",
             new_mode, FRAM_POND_MODE_ADDR);

    // ─── Bước 3: Cập nhật RAM Sys_Info trước khi reboot ───
    Sys_Info.pondMode = new_mode;

    // ─── Bước 4: Reboot để toàn bộ hệ thống khởi động lại sạch ───
    ESP_LOGW("POND_MODE", "Pond Mode switch complete. Rebooting ESP32...");
    User_Device_Save_Runtimes_To_Fram();
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

void prvCleanupSchedulesAtEndOfDay(Device_Handle_t *handle)
{
    static int last_cleanup_yday = -1;
    time_t now = Sys_Info.epochtime;
    if(now < (time_t)1600000000)
    {
        return;
    }

    struct tm now_tm = {0};
    if(localtime_r(&now, &now_tm) == NULL)
    {
        return;
    }

    if(last_cleanup_yday == -1)
    {
        last_cleanup_yday = now_tm.tm_yday;
        return;
    }

    if(now_tm.tm_yday == last_cleanup_yday)
    {
        return;
    }

    last_cleanup_yday = now_tm.tm_yday;

    for(uint8_t i = 0; i < DEVICE_MAX_NUM; i++)
    {
        Device_Parameters_t *dev = &handle->Device[i];
        if(dev->scheduleCount == 0)
        {
            continue;
        }

        uint8_t write_idx = 0;
        for(uint8_t read_idx = 0; read_idx < dev->scheduleCount; read_idx++)
        {
            if(dev->schedules[read_idx].isFinished == 0)
            {
                if(write_idx != read_idx)
                {
                    dev->schedules[write_idx] = dev->schedules[read_idx];
                }
                write_idx++;
            }
        }

        if(write_idx != dev->scheduleCount)
        {
            dev->scheduleCount = write_idx;
            if(dev->scheduleCount == 0)
            {
                dev->scheduleIndex = 0;
                dev->isScheduled = false;
                dev->startTime = 0;
                dev->stopTime = 0;
            }
            else
            {
                if(1)
                {
                    dev->scheduleIndex = 0;
                }
                Device_Schedule_t *cur = &dev->schedules[dev->scheduleIndex];
                dev->isScheduled = true;
                dev->startTime = cur->startTime;
                dev->stopTime = cur->stopTime;
            }

            FRAM_SaveDevice(i);
            ESP_LOGI("SCHEDULE", "Daily cleanup device %u, remaining=%u",
                     (unsigned int)i,
                     (unsigned int)dev->scheduleCount);
        }
    }
}

void User_Device_Init(void)
{
    DeviceHandle.outputBuf = 0x00;
    memset(&DeviceHandle, 0, sizeof(DeviceHandle));

    memcpy(&DeviceHandle.Device[0].name, GROUP_1_DEVICE_NAME_1, strlen(GROUP_1_DEVICE_NAME_1));
    memcpy(&DeviceHandle.Device[1].name, GROUP_1_DEVICE_NAME_2, strlen(GROUP_1_DEVICE_NAME_2));
    memcpy(&DeviceHandle.Device[2].name, GROUP_1_DEVICE_NAME_3, strlen(GROUP_1_DEVICE_NAME_3));
    memcpy(&DeviceHandle.Device[3].name, GROUP_1_DEVICE_NAME_4, strlen(GROUP_1_DEVICE_NAME_4));
    memcpy(&DeviceHandle.Device[4].name, GROUP_2_DEVICE_NAME_1, strlen(GROUP_2_DEVICE_NAME_1));
    memcpy(&DeviceHandle.Device[5].name, GROUP_2_DEVICE_NAME_2, strlen(GROUP_2_DEVICE_NAME_2));
    memcpy(&DeviceHandle.Device[6].name, GROUP_3_DEVICE_NAME_1, strlen(GROUP_3_DEVICE_NAME_1));
    memcpy(&DeviceHandle.Device[7].name, GROUP_3_DEVICE_NAME_2, strlen(GROUP_3_DEVICE_NAME_2));
    memcpy(&DeviceHandle.Device[8].name, GROUP_4_DEVICE_NAME_1, strlen(GROUP_4_DEVICE_NAME_1));
    memcpy(&DeviceHandle.Device[9].name, GROUP_4_DEVICE_NAME_2, strlen(GROUP_4_DEVICE_NAME_2));

    DeviceHandle.Device[0].id =  GROUP_1_DEVICE_ID_1;
    DeviceHandle.Device[1].id =  GROUP_1_DEVICE_ID_2;
    DeviceHandle.Device[2].id =  GROUP_1_DEVICE_ID_3;
    DeviceHandle.Device[3].id =  GROUP_1_DEVICE_ID_4;
    DeviceHandle.Device[4].id =  GROUP_2_DEVICE_ID_1;
    DeviceHandle.Device[5].id =  GROUP_2_DEVICE_ID_2;
    DeviceHandle.Device[6].id =  GROUP_3_DEVICE_ID_1;
    DeviceHandle.Device[7].id =  GROUP_3_DEVICE_ID_2;
    DeviceHandle.Device[8].id =  GROUP_4_DEVICE_ID_1;
    DeviceHandle.Device[9].id =  GROUP_4_DEVICE_ID_2;


    DeviceHandle.Device[0].index = 0;
    DeviceHandle.Device[1].index = 1;
    DeviceHandle.Device[2].index = 2;
    DeviceHandle.Device[3].index = 3;
    DeviceHandle.Device[4].index = 4;
    DeviceHandle.Device[5].index = 5;
    DeviceHandle.Device[6].index = 6;
    DeviceHandle.Device[7].index = 7;
    DeviceHandle.Device[8].index = 8;
    DeviceHandle.Device[9].index = 9;

    DeviceHandle.Device[0].isActived = true;
    DeviceHandle.Device[1].isActived = true;
    DeviceHandle.Device[2].isActived = true;
    DeviceHandle.Device[3].isActived = true;
    DeviceHandle.Device[4].isActived = true;
    DeviceHandle.Device[5].isActived = true;
    DeviceHandle.Device[6].isActived = true;
    DeviceHandle.Device[7].isActived = true;
    DeviceHandle.Device[8].isActived = true;
    DeviceHandle.Device[9].isActived = true;

    // Đọc thời gian chạy tích lũy từ FRAM
    uint32_t saved_runtimes[DEVICE_MAX_NUM] = {0};
    if (Fram_Read_Data(FRAM_DEVICE_RUNTIMES_ADDR, (uint8_t *)saved_runtimes, sizeof(saved_runtimes)))
    {
        for (int i = 0; i < DEVICE_MAX_NUM; i++)
        {
            if (saved_runtimes[i] < 86400)
            {
                DeviceHandle.Device[i].runtime = saved_runtimes[i];
            }
            else
            {
                DeviceHandle.Device[i].runtime = 0;
            }
        }
        ESP_LOGI("OUTPUT", "Restored device runtimes from FRAM");
    }
    else
    {
        for (int i = 0; i < DEVICE_MAX_NUM; i++)
        {
            DeviceHandle.Device[i].runtime = 0;
        }
        ESP_LOGE("OUTPUT", "Failed to read device runtimes from FRAM, defaulted to 0");
    }

    // Đọc thông số điện năng tiêu thụ từ FRAM
    if (!Fram_Read_Data(FRAM_vfd_last_energy_ADDR, (uint8_t *)&vfd_last_energy, sizeof(double)) ||
        isnan(vfd_last_energy) || vfd_last_energy < 0.0 || vfd_last_energy > 100000000.0)
    {
        vfd_last_energy = 0.0;
    }
    if (!Fram_Read_Data(FRAM_VFD_DAILY_ENERGY_ADDR, (uint8_t *)&vfd_daily_energy, sizeof(double)) ||
        isnan(vfd_daily_energy) || vfd_daily_energy < 0.0 || vfd_daily_energy > 100000.0)
    {
        vfd_daily_energy = 0.0;
    }
    ESP_LOGI("OUTPUT", "Restored VFD energy from FRAM: LastCumulative = %.1f kWh, Daily = %.1f kWh",
             vfd_last_energy, vfd_daily_energy);
}

void User_Device_Save_Runtimes_To_Fram(void)
{
    uint32_t runtimes[DEVICE_MAX_NUM] = {0};
    for (int i = 0; i < DEVICE_MAX_NUM; i++)
    {
        runtimes[i] = DeviceHandle.Device[i].runtime;
    }
    Fram_Write_Data(FRAM_DEVICE_RUNTIMES_ADDR, (uint8_t *)runtimes, sizeof(runtimes));
    ESP_LOGI("OUTPUT", "Saved device runtimes to FRAM");
}

void Update_Vfd_Energy(double current_cumulative)
{
    if (current_cumulative >= 0.0)
    {
        if (vfd_last_energy <= 0.0 || current_cumulative < vfd_last_energy)
        {
            vfd_last_energy = current_cumulative;
            Fram_Write_Data(FRAM_vfd_last_energy_ADDR, (uint8_t *)&vfd_last_energy, sizeof(double));
        }
        
        vfd_daily_energy = current_cumulative - vfd_last_energy;
        Fram_Write_Data(FRAM_VFD_DAILY_ENERGY_ADDR, (uint8_t *)&vfd_daily_energy, sizeof(double));
    }
}

void User_Device_Report_Daily_Runtimes_And_Reset(void)
{
    // 1. Tạo snapshot (backup)
    double backup_vfd_energy = vfd_daily_energy;
    uint32_t backup_runtimes[DEVICE_MAX_NUM];
    for (int i = 0; i < DEVICE_MAX_NUM; i++)
    {
        backup_runtimes[i] = DeviceHandle.Device[i].runtime;
    }
    
    // Lưu snapshot vào FRAM
    Fram_Write_Data(FRAM_BACKUP_RUNTIMES_ADDR, (uint8_t *)backup_runtimes, sizeof(backup_runtimes));
    Fram_Write_Data(FRAM_BACKUP_VFD_DAILY_ENERGY_ADDR, (uint8_t *)&backup_vfd_energy, sizeof(double));
    ESP_LOGI("RUNTIME REPORT", "Created daily runtimes and VFD energy backup snapshot in FRAM");

    cJSON *root = cJSON_CreateObject();
    if (root == NULL)
    {
        ESP_LOGE("RUNTIME REPORT", "Failed to create JSON root object");
        return;
    }

    cJSON_AddNumberToObject(root, "Code", TELEMETRY_CODE_REPORT_RUNNING_TIME);
    cJSON_AddNumberToObject(root, "TimeStamp", (double)Sys_Info.epochtime);

    cJSON *dataArray = cJSON_CreateArray();
    if (dataArray != NULL)
    {
        for (int i = 0; i < DEVICE_MAX_NUM; i++)
        {
            if (DeviceHandle.Device[i].isActived)
            {
                cJSON *device = cJSON_CreateObject();
                if (device != NULL)
                {
                    cJSON_AddNumberToObject(device, "DeviceId", DeviceHandle.Device[i].id);
                    cJSON_AddStringToObject(device, "DeviceName", DeviceHandle.Device[i].name);
                    cJSON_AddNumberToObject(device, "RunningTime", backup_runtimes[i]);
                    cJSON_AddItemToArray(dataArray, device);
                }
            }
        }
        cJSON_AddItemToObject(root, "DeviceData", dataArray);
    }

    // Thêm lượng điện tiêu thụ trong ngày vào gói Code 301
    if (Sys_Info.vfdEnabled == 1)
    {
        cJSON_AddNumberToObject(root, "VfdEnergy", round(backup_vfd_energy * 10.0) / 10.0);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str != NULL)
    {
        // 1. Gửi gói tin lên Azure IoT Hub
        PushTelemetry(json_str);
        ESP_LOGI("RUNTIME REPORT", "Pushed daily running time report (Code 301) to Azure queue");

        // 2. Lưu vào phân vùng Flash ngoại vi (/web/rt_history_X.txt) xoay vòng
        uint8_t file_index = 1;
        uint8_t packet_count = 0;
        Fram_Read_Data(FRAM_RUNTIME_FILE_INDEX_ADDR, &file_index, 1);
        Fram_Read_Data(FRAM_RUNTIME_PACKET_COUNT_ADDR, &packet_count, 1);

        if (file_index < 1 || file_index > 5)
        {
            file_index = 1;
        }
        if (packet_count > 6)
        {
            packet_count = 0;
        }

        char log_filepath[64];
        snprintf(log_filepath, sizeof(log_filepath), "/web/rt_history_%d.txt", file_index);

        FILE *f = NULL;
        if (packet_count == 0)
        {
            f = fopen(log_filepath, "w");
        }
        else
        {
            f = fopen(log_filepath, "a");
        }

        if (f != NULL)
        {
            fprintf(f, "%s\n", json_str);
            fclose(f);
            ESP_LOGI("RUNTIME REPORT", "Logged runtime history to SPI Flash: %s (packet %d)", log_filepath, packet_count + 1);
        }
        else
        {
            ESP_LOGE("RUNTIME REPORT", "Failed to open SPI Flash file '%s'", log_filepath);
        }

        // Tăng đếm và xoay vòng
        packet_count++;
        if (packet_count >= 7)
        {
            packet_count = 0;
            file_index++;
            if (file_index > 5)
            {
                file_index = 1;
            }
        }

        Fram_Write_Data(FRAM_RUNTIME_FILE_INDEX_ADDR, &file_index, 1);
        Fram_Write_Data(FRAM_RUNTIME_PACKET_COUNT_ADDR, &packet_count, 1);

        free(json_str);
    }
    cJSON_Delete(root);
}

void User_Device_Ack_Daily_Runtimes(void)
{
    uint32_t backup_runtimes[DEVICE_MAX_NUM] = {0};
    double backup_vfd_energy = 0.0;
    
    // 1. Đọc snapshot từ FRAM
    bool read_runtimes = Fram_Read_Data(FRAM_BACKUP_RUNTIMES_ADDR, (uint8_t *)backup_runtimes, sizeof(backup_runtimes));
    bool read_energy = Fram_Read_Data(FRAM_BACKUP_VFD_DAILY_ENERGY_ADDR, (uint8_t *)&backup_vfd_energy, sizeof(double));
    
    if (!read_runtimes || !read_energy || isnan(backup_vfd_energy) || backup_vfd_energy < 0.0 || backup_vfd_energy > 100000.0)
    {
        ESP_LOGE("RUNTIME REPORT", "ACK 117 failed: Invalid backup data in FRAM");
        return;
    }
    
    // 2. Trừ bớt lượng thời gian chạy đã gửi thành công từ chỉ số active hiện tại
    for (int i = 0; i < DEVICE_MAX_NUM; i++)
    {
        if (DeviceHandle.Device[i].runtime >= backup_runtimes[i])
        {
            DeviceHandle.Device[i].runtime -= backup_runtimes[i];
        }
        else
        {
            DeviceHandle.Device[i].runtime = 0;
        }
    }
    User_Device_Save_Runtimes_To_Fram(); // Lưu active runtimes mới xuống FRAM
    
    // 3. Điều chỉnh chỉ số điện năng tiêu thụ
    if (Sys_Info.vfdEnabled == 1 && backup_vfd_energy > 0.0)
    {
        // Chốt điện năng tích lũy ngày cũ tiến lên bằng chỉ số của lượng điện đã ack
        vfd_last_energy += backup_vfd_energy;
        Fram_Write_Data(FRAM_vfd_last_energy_ADDR, (uint8_t *)&vfd_last_energy, sizeof(double));
        
        // Trừ lượng điện tiêu thụ trong ngày hiện tại
        if (vfd_daily_energy >= backup_vfd_energy)
        {
            vfd_daily_energy -= backup_vfd_energy;
        }
        else
        {
            vfd_daily_energy = 0.0;
        }
        Fram_Write_Data(FRAM_VFD_DAILY_ENERGY_ADDR, (uint8_t *)&vfd_daily_energy, sizeof(double));
    }
    
    // 4. Reset snapshot lưu trong FRAM về 0
    memset(backup_runtimes, 0, sizeof(backup_runtimes));
    double zero_val = 0.0;
    Fram_Write_Data(FRAM_BACKUP_RUNTIMES_ADDR, (uint8_t *)backup_runtimes, sizeof(backup_runtimes));
    Fram_Write_Data(FRAM_BACKUP_VFD_DAILY_ENERGY_ADDR, (uint8_t *)&zero_val, sizeof(double));
    
    ESP_LOGI("RUNTIME REPORT", "Processed ACK 117: Reset reported daily runtimes and VFD energy");
}

void User_Device_Report(void)
{
    uint8_t *rpData = (uint8_t *)calloc(2048, sizeof(uint8_t));
    if (rpData == NULL)
    {
        ESP_LOGE("DEVICE REPORT", "Do not enough RAM for report data");
        return;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL)
    {
        ESP_LOGE("DEVICE REPORT", "Do not enough RAM for json");
        free(rpData);
        return;
    }

    cJSON_AddNumberToObject(root, "Code", 300);
    cJSON_AddNumberToObject(root, "TimeStamp", Sys_Info.epochtime);

    cJSON *dataArray = cJSON_CreateArray();
    if (dataArray != NULL)
    {
        for (int i = 0; i < DEVICE_MAX_NUM; i++)
        {        
            if (DeviceHandle.Device[i].isActived)
            {
                cJSON *device = cJSON_CreateObject();
                if (device != NULL)
                {
                    cJSON_AddStringToObject(device, "DeviceName", DeviceHandle.Device[i].name);
                    cJSON_AddNumberToObject(device, "DeviceId", DeviceHandle.Device[i].id);
                    cJSON_AddStringToObject(device, "Status", (DeviceHandle.Device[i].state == 0) ? "OFF" : ((DeviceHandle.Device[i].state == 1) ? "ON" : "FAULT"));

                    if (strncmp(DeviceHandle.Device[i].name, "Feeder_", strlen("Feeder_")) == 0)
                    {
                        cJSON_AddNumberToObject(device, "Weight", DeviceHandle.Device[i].weight);
                    }

                    cJSON_AddNumberToObject(device, "Index", DeviceHandle.Device[i].index);
                    cJSON_AddItemToArray(dataArray, device);
                }
            }
        }
        cJSON_AddItemToObject(root, "DeviceData", dataArray);
    }

    // 2. Đóng gói VfdData (Object)
    cJSON *vfdObj = cJSON_CreateObject();
    if (vfdObj != NULL)
    {
        if (Sys_Info.vfdEnabled == 1)
        {
            GD200A_Status_t vfd_status;
            RS485_Status_t ret = GD200A_ReadStatus(GD200A_SLAVE_ADDR, &vfd_status);

            if (ret == RS485_OK)
            {
                Update_Vfd_Energy(vfd_status.cumulative_energy_kwh);
                
                cJSON_AddBoolToObject(vfdObj, "IsConnected", true);
                cJSON_AddStringToObject(vfdObj, "Mode", (vfd_status.run_command_channel == 2) ? "auto" : "manual");
                cJSON_AddBoolToObject(vfdObj, "IsRunning", vfd_status.is_running);
                cJSON_AddBoolToObject(vfdObj, "IsFwd", vfd_status.is_fwd);
                cJSON_AddBoolToObject(vfdObj, "IsFault", vfd_status.is_fault);
                cJSON_AddNumberToObject(vfdObj, "FaultCode", vfd_status.fault_code);
                cJSON_AddStringToObject(vfdObj, "FaultString", GD200A_GetFaultString(vfd_status.fault_code));
                cJSON_AddNumberToObject(vfdObj, "ActualFreq", round(vfd_status.freq_actual_hz * 10.0) / 10.0);
                cJSON_AddNumberToObject(vfdObj, "SetFreq", round(vfd_status.freq_set_hz * 10.0) / 10.0);
                cJSON_AddNumberToObject(vfdObj, "AccelTime", round(vfd_status.accel_time_s * 10.0) / 10.0);
                cJSON_AddNumberToObject(vfdObj, "DecelTime", round(vfd_status.decel_time_s * 10.0) / 10.0);
                cJSON_AddNumberToObject(vfdObj, "OutputCurrent", round(vfd_status.output_current_a * 10.0) / 10.0);
                cJSON_AddNumberToObject(vfdObj, "OutputVoltage", vfd_status.output_voltage_v);
                cJSON_AddNumberToObject(vfdObj, "BusVoltage", round(vfd_status.bus_voltage_v * 10.0) / 10.0);
                cJSON_AddNumberToObject(vfdObj, "MotorSpeed", vfd_status.motor_speed_rpm);
                cJSON_AddNumberToObject(vfdObj, "OutputPower", round(vfd_status.output_power_pct * 10.0) / 10.0);
                cJSON_AddBoolToObject(vfdObj, "AutoRunEnable", vfd_status.auto_run_enable == 1);
                cJSON_AddNumberToObject(vfdObj, "AutoRunDelay", round(vfd_status.auto_run_delay_s * 10.0) / 10.0);
            }
            else
            {
                cJSON_AddBoolToObject(vfdObj, "IsConnected", false);
                cJSON_AddNumberToObject(vfdObj, "ErrorCode", (int)ret);
                cJSON_AddStringToObject(vfdObj, "Messsage", "485 cable breakage or VFD power loss");
                ESP_LOGW("VFD REPORT", "Failed to read VFD status (err=%d)", (int)ret);
            }
        }
        // Nếu vfdEnabled == 0, vfdObj vẫn là một đối tượng rỗng {} được cJSON_CreateObject() tạo ra
        cJSON_AddItemToObject(root, "VfdData", vfdObj);
    }

    if (cJSON_PrintPreallocated(root, (char *)rpData, 2048, false) != true)
    {
        ESP_LOGE("DEVICE REPORT", "Do not enough RAM for render json");
        cJSON_Delete(root);
        free(rpData);
        return;
    }

    ESP_LOGI("", "\n\n");
    ESP_LOGI("REPORT", "---------- Azure report data ----------");
    
    /* Tạo chuỗi màu cho giao diện terminal log (không làm thay đổi data truyền lên Azure) */
    char *colored_rpData = malloc(4096);
    if (colored_rpData)
    {
        char *d = colored_rpData;
        const char *s = (const char *)rpData;
        while (*s)
        {
            if (strncmp(s, "\"ON\"", 4) == 0)
            {
                strcpy(d, "\033[1;32m\"ON\"\033[0;32m");
                d += strlen("\033[1;32m\"ON\"\033[0;32m"); 
                s += 4;
            }
            else if (strncmp(s, "\"OFF\"", 5) == 0)
            {
                strcpy(d, "\033[1;33m\"OFF\"\033[0;32m");
                d += strlen("\033[1;33m\"OFF\"\033[0;32m"); 
                s += 5;
            }
            else if (strncmp(s, "\"FAULT\"", 7) == 0)
            {
                strcpy(d, "\033[1;31m\"FAULT\"\033[0;32m");
                d += strlen("\033[1;31m\"FAULT\"\033[0;32m"); 
                s += 7;
            }
            else *d++ = *s++;
        }
        *d = '\0';
        ESP_LOGI("REPORT", "%s\n\n", colored_rpData);
        free(colored_rpData);
    }
    else
    {
        ESP_LOGI("REPORT", "%s\n\n", rpData);
    }

    ESP_LOGI("REPORT", "---------- Scheduled list ----------");
    for (int i = 0; i < DEVICE_MAX_NUM; i++)
    {
        if (!DeviceHandle.Device[i].isActived)
        {
            continue;
        }

        uint8_t count = DeviceHandle.Device[i].scheduleCount;
        if (count == 0)
        {
            continue;
        }

        if (count > DEVICE_SCHEDULE_MAX)
        {
            count = DEVICE_SCHEDULE_MAX;
        }

        for (uint8_t j = 0; j < count; j++)
        {
            Device_Schedule_t *sch = &DeviceHandle.Device[i].schedules[j];
            char start_str[20] = "N/A";
            char stop_str[20] = "N/A";
            struct tm start_tm = {0};
            struct tm stop_tm = {0};
            time_t start_epoch = sch->startTime;
            time_t stop_epoch = sch->stopTime;

            if (start_epoch > (time_t)1600000000 && start_epoch < (time_t)4102444800)
            {
                if (localtime_r(&start_epoch, &start_tm) != NULL)
                {
                    strftime(start_str, sizeof(start_str), "%d/%m/%Y %H:%M:%S", &start_tm);
                }
            }

            if (stop_epoch > (time_t)1600000000 && stop_epoch < (time_t)4102444800)
            {
                if (localtime_r(&stop_epoch, &stop_tm) != NULL)
                {
                    strftime(stop_str, sizeof(stop_str), "%d/%m/%Y %H:%M:%S", &stop_tm);
                }
            }

            ESP_LOGI("REPORT",
                     "Schedule_index %u --> Name: %s, ID: %d, StartTime: %s, StopTime: %s, IsFinish: %u",
                     (unsigned int)j,
                     DeviceHandle.Device[i].name,
                     DeviceHandle.Device[i].id,
                     start_str,
                     stop_str,
                     (unsigned int)sch->isFinished);
        }
    }
//**********************REPORT RUNNING TIME & RESET COUNT********************//
    {
        int64_t uptime_us = esp_timer_get_time();
        uint32_t total_sec = (uint32_t)(uptime_us / 1000000ULL);
        uint32_t h = total_sec / 3600;
        uint32_t m = (total_sec % 3600) / 60;
        uint32_t s = total_sec % 60;
        ESP_LOGI("REPORT", "Running Time  : %u:%02u:%02u", (unsigned int)h, (unsigned int)m, (unsigned int)s);
        ESP_LOGI("REPORT", "Reset Count   : %u", (unsigned)reset_count);
    }
//**********************REPORT RUNNING TIME & RESET COUNT********************//
    PushTelemetry((const char *)rpData);

    cJSON_Delete(root);
    free(rpData);
}

void User_Out_Put_IO_Config(void)
{
   gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = GPIO_OUTPUT_PIN_SEL;
    io_conf.pull_down_en = 1;
    io_conf.pull_up_en = 0;
    gpio_config(&io_conf);

    gpio_set_level(OUTPUT_ENABLE_PIN, 0); // Set output enable pin low to enable output
}

void User_Input_IO_Config(void)
{
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = GPIO_INPUT_PIN_SEL;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;

    gpio_config(&io_conf);
}

void User_Output_Shift_Data(const uint32_t *data)
{
    uint32_t mask = 0x40000000;
    
    for(int i = 0; i < 32; i++)
    {
        mask = 0x80000000 >> i;
        if(*data & (mask))
        {
            gpio_set_level(IO_MOSI_PIN, 1);
        }
        else
        {
            
            gpio_set_level(IO_MOSI_PIN, 0);
        }
        IO_CLK_HIGH();
        IO_CLK_LOW();
    }

    IO_STR_HIGH();
    IO_STR_LOW();
}

void User_Output_Parse_Buffer(Device_Handle_t *handle)
{
    for(uint8_t i = 0; i < DEVICE_MAX_NUM; i++)
    {
        if(!handle->Device[i].isActived)
        {
            handle->outputBuf &= ~(1UL << i);
            handle->Device[i].state = DEVICE_STATE_OFF;
            continue;
        }
        // Get device state from output buffer to check on or off
        if(handle->outputBuf & (1UL<<i))
        {
            handle->Device[i].state = DEVICE_STATE_ON;
        }
        else
        {
            handle->Device[i].state = DEVICE_STATE_OFF;
        }
    }

    handle->isTriggered = true;
}

void User_Output_Deploy(Device_Handle_t *handle)
{
    if(handle->isTriggered)
    {
        // Shift data
        uint32_t mask = 0x40000000;
        for(int i = 0; i < 32; i++)
        {
            mask = 0x80000000 >> i;
            if(handle->outputBuf & (mask))
            {
                if(handle->Device[i].isScheduled)
                {
                    handle->Device[i].stopTime = handle->Device[i].startTime + handle->Device[i].duration;
                }
                else
                {
                    gpio_set_level(IO_MOSI_PIN, 1);
                }
            }
            else
            {
                gpio_set_level(IO_MOSI_PIN, 0);
            }

            IO_CLK_HIGH();
            IO_CLK_LOW();
        }

        IO_STR_HIGH();
        IO_STR_LOW();
    }
    
}

// static uint16_t prvGetDefaultDeviceIdByIndex(uint8_t index)
// {
//     static const uint16_t default_ids[DEVICE_MAX_NUM] = {
//         GROUP_1_DEVICE_ID_1, GROUP_1_DEVICE_ID_2, GROUP_1_DEVICE_ID_3, GROUP_1_DEVICE_ID_4,
//         GROUP_2_DEVICE_ID_1, GROUP_2_DEVICE_ID_2,
//         GROUP_3_DEVICE_ID_1, GROUP_3_DEVICE_ID_2,
//         GROUP_4_DEVICE_ID_1, GROUP_4_DEVICE_ID_2,};

//     if(index < DEVICE_MAX_NUM)
//     {
//         return default_ids[index];
//     }

//     return 0;
// }

// static const char *prvGetDefaultDeviceNameByIndex(uint8_t index)
// {
//     static const char *default_names[DEVICE_MAX_NUM] =
//     {
//         GROUP_1_DEVICE_NAME_1, GROUP_1_DEVICE_NAME_2, GROUP_1_DEVICE_NAME_3, GROUP_1_DEVICE_NAME_4,
//         GROUP_2_DEVICE_NAME_1, GROUP_2_DEVICE_NAME_2,
//         GROUP_3_DEVICE_NAME_1, GROUP_3_DEVICE_NAME_2,
//         GROUP_4_DEVICE_NAME_1, GROUP_4_DEVICE_NAME_2
//     };

//     if(index < DEVICE_MAX_NUM)
//     {
//         return default_names[index];
//     }

//     return "";
// }

static int prvGetDeviceIndexById(uint16_t deviceId)
{
    for(int i = 0; i < DEVICE_MAX_NUM; i++)
    {
        if(DeviceHandle.Device[i].id == deviceId)
        {
            return i;
        }
    }

    return -1;
}

static void prvSetDeviceOnOff(Device_Handle_t *handle, uint8_t index, bool on, const char *tag)
{
    if(handle->Device[index].id == GROUP_3_DEVICE_ID_2 && isManualFeeder_M2) return;

    if(on)
    {
        if(handle->Device[index].state != DEVICE_STATE_ON)
        {
            User_Output_Set_Value(1, index, handle);
            handle->Device[index].state = DEVICE_STATE_ON;
            if(tag != NULL)
            {
                ESP_LOGI("FEEDER", "%s ON (idx %d)", tag, index);
            }
        }
    }
    else
    {
        if(handle->Device[index].state != DEVICE_STATE_OFF)
        {
            User_Output_Set_Value(0, index, handle);
            handle->Device[index].state = DEVICE_STATE_OFF;
            if(tag != NULL)
            {
                ESP_LOGI("FEEDER", "%s OFF (idx %d)", tag, index);
            }
        }
    }
}

static void prvAdvanceSchedule(Device_Parameters_t *dev)
{
    if(dev->scheduleCount == 0)
    {
        dev->isScheduled = false;
        return;
    }

    if(dev->scheduleIndex >= dev->scheduleCount)
    {
        dev->isScheduled = false;
        return;
    }

    dev->schedules[dev->scheduleIndex].isFinished = 1;

    for(uint8_t i = (uint8_t)(dev->scheduleIndex + 1); i < dev->scheduleCount; i++)
    {
        if(dev->schedules[i].isFinished == 0)
        {
            dev->scheduleIndex = i;
            dev->startTime = dev->schedules[i].startTime;
            dev->stopTime = dev->schedules[i].stopTime;
            dev->duration = (dev->stopTime > dev->startTime) ? (uint32_t)(dev->stopTime - dev->startTime) : 0;
            dev->isScheduled = true;
            FRAM_SaveDevice(dev->index);
            return;
        }
    }

    dev->isScheduled = false;
    FRAM_SaveDevice(dev->index);
}

static void prvHandleFeederSchedule(Device_Handle_t *handle, uint8_t m1Index, uint8_t m2Index)
{
    if(handle->Device[m2Index].scheduleCount == 0)
    {
        handle->Device[m2Index].scheduleCount = 1;
        handle->Device[m2Index].scheduleIndex = 0;
        handle->Device[m2Index].schedules[0].startTime = handle->Device[m2Index].startTime;
        handle->Device[m2Index].schedules[0].stopTime = handle->Device[m2Index].stopTime;
        handle->Device[m2Index].schedules[0].isFinished = 0;
    }

    time_t now = Sys_Info.epochtime;
    time_t start = handle->Device[m2Index].startTime;
    time_t stop = handle->Device[m2Index].stopTime;

    if(now >= stop)
    {
        prvSetDeviceOnOff(handle, m1Index, false, "M1");
        prvSetDeviceOnOff(handle, m2Index, false, "M2");
        handle->Device[m2Index].schedules[handle->Device[m2Index].scheduleIndex].isFinished = 1;
        prvAdvanceSchedule(&handle->Device[m2Index]);
        ESP_LOGI("FEEDER", "Feeder schedule finished (idx %d)", m2Index);
        return;
    }

    time_t m2Start = (start >= 3) ? (start - 3) : 0;
    
    // Nếu sắp tới giờ Lịch trình chạy (nằm trong cửa sổ hoạt động), TỰ ĐỘNG GỠ CỜ MANUAL để nhường quyền cho lịch
    if(now >= m2Start && now < stop)
    {
        isManualFeeder_M2 = false;
    }

    if(now < m2Start)
    {
        prvSetDeviceOnOff(handle, m1Index, false, NULL);
        prvSetDeviceOnOff(handle, m2Index, false, NULL);
        return;
    }

    bool m1_on = (now >= start) && (now < stop);
    bool m2_on = (now >= m2Start) && (now < stop);
    prvSetDeviceOnOff(handle, m1Index, m1_on, "M1");
    prvSetDeviceOnOff(handle, m2Index, m2_on, "M2");
}

/* --- Feeder M2 Manual Control with delayed M1 --- */
typedef enum { FEEDER_PENDING_NONE = 0, FEEDER_PENDING_ON, FEEDER_PENDING_OFF } feeder_pending_t;
static feeder_pending_t s_feeder_m1_pending = FEEDER_PENDING_NONE;
static TickType_t       s_feeder_m1_tick    = 0;

void User_Feeder_M2_Manual_Control(bool on)
{
    int m2Index = prvGetDeviceIndexById(GROUP_3_DEVICE_ID_2);
    int m1Index = prvGetDeviceIndexById(GROUP_3_DEVICE_ID_1);

    if (Sys_Info.feederMode == 1)
    {
        // Chế độ 1 động cơ: Chỉ điều khiển đúng động cơ được chọn làm máy cho ăn chính
        int activeIdx = (Sys_Info.activeFeederId == 31) ? m1Index : m2Index;
        if (activeIdx >= 0)
        {
            if (on)
            {
                DeviceHandle.outputBuf |= (1UL << activeIdx);
                DeviceHandle.Device[activeIdx].state = DEVICE_STATE_ON;
            }
            else
            {
                DeviceHandle.outputBuf &= ~(1UL << activeIdx);
                DeviceHandle.Device[activeIdx].state = DEVICE_STATE_OFF;
            }
            User_Output_Set_Value(on ? 1 : 0, (uint8_t)activeIdx, &DeviceHandle);
            FRAM_SaveDevice((uint8_t)activeIdx);
            ESP_LOGI("FEEDER", "1-Motor Mode: Manual %s for Active Feeder ID %d", on ? "ON" : "OFF", Sys_Info.activeFeederId);
        }
        isManualFeeder_M2 = false; // Gỡ cờ manual chung
        return;
    }

    if(on)
    {
        /* Bật M2 trước, sau 3s sẽ bật M1 */
        if(m2Index >= 0)
        {
            DeviceHandle.outputBuf |= (1UL << m2Index);
            DeviceHandle.Device[m2Index].state = DEVICE_STATE_ON;
            isManualFeeder_M2 = true;
            ESP_LOGI("FEEDER", "Manual ON: M2 started, M1 pending in 3s");
        }
        s_feeder_m1_pending = FEEDER_PENDING_ON;
        s_feeder_m1_tick    = xTaskGetTickCount() + pdMS_TO_TICKS(3000);
        (void)m1Index;
    }
    else
    {
        /* Tắt M1 trước, sau 3s sẽ tắt M2 */
        if(m1Index >= 0)
        {
            DeviceHandle.outputBuf &= ~(1UL << m1Index);
            User_Output_Set_Value(0, (uint8_t)m1Index, &DeviceHandle);
            DeviceHandle.Device[m1Index].state = DEVICE_STATE_OFF;
            ESP_LOGI("FEEDER", "Manual OFF: M1 stopped, M2 pending off in 3s");
        }
        s_feeder_m1_pending = FEEDER_PENDING_OFF;
        s_feeder_m1_tick    = xTaskGetTickCount() + pdMS_TO_TICKS(3000);
        (void)m2Index;
    }
}

void User_Output_Polling(Device_Handle_t *handle)
{
    static uint8_t AirBLow_1 = 0;
    static uint8_t AirBlow_2 = 0;

    /* Xử lý pending ON/OFF của Feeder M1 (delay 3s sau khi M2 thay đổi trạng thái) */
    if(Sys_Info.feederMode == 0 && s_feeder_m1_pending != FEEDER_PENDING_NONE && xTaskGetTickCount() >= s_feeder_m1_tick)
    {
        int m1Idx = prvGetDeviceIndexById(GROUP_3_DEVICE_ID_1);
        int m2Idx = prvGetDeviceIndexById(GROUP_3_DEVICE_ID_2);
        if(s_feeder_m1_pending == FEEDER_PENDING_ON)
        {
            if(m1Idx >= 0)
            {
                handle->outputBuf |= (1UL << m1Idx);
                User_Output_Set_Value(1, (uint8_t)m1Idx, handle);
                handle->Device[m1Idx].state = DEVICE_STATE_ON;
                ESP_LOGI("FEEDER", "Manual ON: M1 started (delayed)");
            }
        }
        else if(s_feeder_m1_pending == FEEDER_PENDING_OFF)
        {
            if(m2Idx >= 0)
            {
                handle->outputBuf &= ~(1UL << m2Idx);
                User_Output_Set_Value(0, (uint8_t)m2Idx, handle);
                handle->Device[m2Idx].state = DEVICE_STATE_OFF;
                isManualFeeder_M2 = false;
                ESP_LOGI("FEEDER", "Manual OFF: M2 stopped (delayed)");
            }
        }
        s_feeder_m1_pending = FEEDER_PENDING_NONE;
    }

    prvCleanupSchedulesAtEndOfDay(handle);  

    if(handle->activeType != DEVICE_ACTIVE_TYPE_NONE)
    {
        if(handle->activeType == DEVICE_ACTIVE_TYPE_TRIGGER)
        {
            User_Output_Shift_Data(&handle->outputBuf);
            handle->activeType = DEVICE_ACTIVE_TYPE_NONE;

            if((handle->outputBuf & (1UL<<4)))
            {
                AirBLow_1 = 1;
                ESP_LOGI("TEST: ", "Test");
            }
            else AirBLow_1 = 0;
            
            if((handle->outputBuf & (1UL<<5)))
            {
                AirBlow_2 = 1;
                ESP_LOGI("TEST: ", "Test");
            }
            else AirBlow_2 = 0;
 
                
        }
    }

    for(int i = 0; i < DEVICE_MAX_NUM; i++)
    {
        if(handle->Device[i].isScheduled)
        {
            // Bỏ qua nếu lịch trình đang bị tạm dừng để điều khiển thủ công (CMD 111)
            if(handle->Device[i].isSchedulePaused)
            {
                continue;
            }

            if (Sys_Info.feederMode == 0)
            {
                if(handle->Device[i].id == GROUP_3_DEVICE_ID_1)
                {
                    // Feeder_1_M1 is controlled by Feeder_1_M2 schedule.
                    continue;
                }
                if(handle->Device[i].id == GROUP_3_DEVICE_ID_2)
                {
                    int m1Index = prvGetDeviceIndexById(GROUP_3_DEVICE_ID_1);
                    if(m1Index >= 0)
                    {
                        prvHandleFeederSchedule(handle, (uint8_t)m1Index, (uint8_t)i);
                        continue;
                    }
                }
            }

            if (Sys_Info.traditionalOxyEnabled == 1)
            {
                if (Sys_Info.oxyMode == 0)
                {
                    if(handle->Device[i].id == GROUP_2_DEVICE_ID_1 || handle->Device[i].id == GROUP_2_DEVICE_ID_2)
                    {
                        continue; // Chế độ 2 máy: Cả hai máy đều là máy oxy, không chạy lịch trình tự động
                    }
                }
                else if (Sys_Info.oxyMode == 1)
                {
                    if(handle->Device[i].id == Sys_Info.activeOxyId)
                    {
                        continue; // Máy oxy chính ở chế độ 1 máy, không chạy lịch trình
                    }
                }
            }

            time_t now = Sys_Info.epochtime;

            if(now < handle->Device[i].startTime)
            {
                continue;
            }

            if(now >= handle->Device[i].stopTime)
            {
                if(handle->Device[i].state == DEVICE_STATE_ON)
                {
                    User_Output_Set_Value(0, i, handle);
                    handle->outputBuf &= ~(1UL<<i);
                }

                handle->Device[i].state = DEVICE_STATE_OFF;
                prvAdvanceSchedule(&handle->Device[i]);

                ESP_LOGI("OUTPUT", "Device %d schedule finished", i);
                continue;
            }

            if(handle->Device[i].state != DEVICE_STATE_ON)
            {
                User_Output_Set_Value(1, i, handle);
                handle->outputBuf |= (1UL<<i);
                handle->Device[i].state = DEVICE_STATE_ON;
                ESP_LOGI("OUTPUT", "Device %d ON schedule", i);
            }
        }
    }

    if(User_Device_Get_Error_Status((uint8_t *)&handle->inputBuf, 2) == ESP_OK)
    {
        handle->isInputChecked = true;
    }
    else
    {
        handle->isInputChecked = false;
    }

    static TickType_t fault_start_ticks[DEVICE_MAX_NUM] = {0};
    static bool is_fault_started[DEVICE_MAX_NUM] = {false};

    for(int i = 0; i < DEVICE_MAX_NUM; i++)
    {
        // Phục hồi trạng thái FAULT nếu đã được lưu, nếu không thì lấy trạng thái ON/OFF từ outputBuf
        if(device_fault_states & (1UL<<i))
        {
            handle->Device[i].state = DEVICE_STATE_FAULT;
        }
        else if(handle->outputBuf & (1UL<<i))
        {
            handle->Device[i].state = DEVICE_STATE_ON;
        }
        else
        {
            handle->Device[i].state = DEVICE_STATE_OFF;
        }

        if(handle->isInputChecked) //If get input from expander successfully then update device state, else do nothing
        {
            bool is_potential_fault = false;
            if(i == 4)
            {
                if (Sys_Info.traditionalOxyEnabled == 1)
                {
                    // Máy oxy 1 (ID 21): Ở Mode 4 ao check cặp chân 4 & 10. Ở Mode 10 ao check 4 & 10 (hoặc theo activeOxyId)
                    if (Sys_Info.pondMode == POND_MODE_4_DEV)
                    {
                        is_potential_fault = (((handle->inputBuf & (1UL << 4)) == 0) && ((handle->inputBuf & (1UL << 10)) == 0));
                    }
                    else if (Sys_Info.oxyMode == 0 || (Sys_Info.oxyMode == 1 && Sys_Info.activeOxyId == 21))
                    {
                        is_potential_fault = (((handle->inputBuf & (1UL << 4)) == 0) && ((handle->inputBuf & (1UL << 10)) == 0));
                    }
                    else
                    {
                        // Chạy như thiết bị thường (chỉ check chân 4)
                        is_potential_fault = ((handle->inputBuf & (1UL << 4)) == 0);
                    }
                }
                else
                {
                    is_potential_fault = ((handle->inputBuf & (1UL << 4)) == 0);
                }
            }
            else if(i == 5)
            {
                if (Sys_Info.traditionalOxyEnabled == 1)
                {
                    // Máy oxy 2 (ID 22): Logic máy oxy (check chân 5 & 11) chạy khi oxyMode = 0 HOẶC khi oxyMode = 1 và máy 2 là máy oxy chính
                    if (Sys_Info.oxyMode == 0 || (Sys_Info.oxyMode == 1 && Sys_Info.activeOxyId == 22))
                    {
                        is_potential_fault = (((handle->inputBuf & (1UL << 5)) == 0) && ((handle->inputBuf & (1UL << 11)) == 0));
                    }
                    else
                    {
                        // Chạy như thiết bị thường (chỉ check chân 5)
                        is_potential_fault = ((handle->inputBuf & (1UL << 5)) == 0);
                    }
                }
                else
                {
                    is_potential_fault = ((handle->inputBuf & (1UL << 5)) == 0);
                }
            }
            else
            {
                // Các thiết bị khác: Lỗi khi bit index tương ứng = 0
                is_potential_fault = ((handle->inputBuf & (1UL << i)) == 0);
            }

            if(is_potential_fault) // Potential fault
            {
                if(!is_fault_started[i])
                {
                    is_fault_started[i] = true;
                    fault_start_ticks[i] = xTaskGetTickCount();
                }
                
                // Nếu đã lưu là FAULT từ trước khi reset, hoặc đếm đủ 10s
                if((device_fault_states & (1UL<<i)) || ((xTaskGetTickCount() - fault_start_ticks[i]) >= pdMS_TO_TICKS(10000)))
                {
                    handle->Device[i].state = DEVICE_STATE_FAULT;
                    device_fault_states |= (1UL<<i);
                }
            } 
            else
            {
                is_fault_started[i] = false;
                device_fault_states &= ~(1UL<<i);
            }
        }                                      
    }
/////////////////////////////////////// Cơ chế bật tắt máy oxy dự phòng và quạt nước khi lỗi /////////////////////////
    // static int airblow2Index = -1;
    // static bool airblow_pending = false;
    // static TickType_t airblow_apply_tick = 0;
    // static bool pending_airblow1 = false;
    // static bool pending_airblow2 = false;
    // static bool both_fault_prev = false;
    // static bool airblow1_auto_triggered = false;
    // static bool airblow2_auto_triggered = false;

    // if(airblow1Index < 0)
    // {
    //     airblow1Index = prvGetDeviceIndexById(GROUP_2_DEVICE_ID_1);
    // }
    // if(airblow2Index < 0)
    // {
    //     airblow2Index = prvGetDeviceIndexById(GROUP_2_DEVICE_ID_2);
    // }

    // if(airblow1Index >= 0)
    // {
    //     handle->Device[airblow1Index].isScheduled = false;
    // }

    // if(airblow1Index >= 0 && airblow2Index >= 0)
    // {
    //     if(handle->isInputChecked)
    //     {
    //         bool fault1 = (handle->Device[airblow1Index].state == DEVICE_STATE_FAULT);
    //         bool fault2 = (handle->Device[airblow2Index].state == DEVICE_STATE_FAULT);
    //         bool desired1 = false;
    //         bool desired2 = false;
    //         bool both_fault = fault1 && fault2;

    //         bool cur1 = (handle->outputBuf & (1UL << airblow1Index)) != 0;
    //         bool cur2 = (handle->outputBuf & (1UL << airblow2Index)) != 0;

    //         if(both_fault)
    //         {
    //             desired1 = false;
    //             desired2 = false;
    //         }
    //         else if(!fault1 && fault2)
    //         {
    //             desired1 = true;
    //             desired2 = false;
                
    //             if(!cur1) {
    //                 airblow1_auto_triggered = true;
    //             }
    //             airblow2_auto_triggered = false;
    //         }
    //         else if(fault1 && !fault2)
    //         {
    //             desired1 = false;
    //             desired2 = true;
                
    //             if(!cur2) {
    //                 airblow2_auto_triggered = true;
    //             }
    //             airblow1_auto_triggered = false;
    //         }
    //         else
    //         {
    //             if(airblow1_auto_triggered)
    //             {
    //                 desired1 = false;
    //                 desired2 = true;
    //                 if(!cur1) {
    //                     airblow1_auto_triggered = false;
    //                 }
    //             }
    //             else if(airblow2_auto_triggered)
    //             {
    //                 desired1 = true;
    //                 desired2 = false;
    //                 if(!cur2) {
    //                     airblow2_auto_triggered = false;
    //                 }
    //             }
    //             else
    //             {
    //                 desired1 = cur1; // Allow manual control when healthy
    //                 desired2 = cur2;
    //             }
    //         }

    //         if(both_fault && !both_fault_prev)
    //         {
    //             for(int i = 0; i < 4; i++)
    //             {
    //                 if(handle->Device[i].state != DEVICE_STATE_ON)
    //                 {
    //                     User_Output_Set_Value(1, (uint8_t)i, handle);
    //                     handle->Device[i].state = DEVICE_STATE_ON;
    //                 }
    //             }
    //         }
    //         else if(!both_fault && both_fault_prev)
    //         {
    //             for(int i = 0; i < 4; i++)
    //             {
    //                 if(handle->Device[i].state != DEVICE_STATE_OFF)
    //                 {
    //                     User_Output_Set_Value(0, (uint8_t)i, handle);
    //                     handle->Device[i].state = DEVICE_STATE_OFF;
    //                 }
    //             }
    //         }

    //         if((desired1 != cur1) || (desired2 != cur2))
    //         {
    //             TickType_t now = xTaskGetTickCount();
    //             if(!airblow_pending || pending_airblow1 != desired1 || pending_airblow2 != desired2)
    //             {
    //                 airblow_pending = true;
    //                 pending_airblow1 = desired1;
    //                 pending_airblow2 = desired2;
    //                 airblow_apply_tick = now + pdMS_TO_TICKS(2000);
    //             }
    //         }

    //         if(airblow_pending && xTaskGetTickCount() >= airblow_apply_tick)
    //         {
    //             if(pending_airblow1 != cur1)
    //             {
    //                 User_Output_Set_Value(pending_airblow1 ? 1 : 0, (uint8_t)airblow1Index, handle);
    //             }
    //             if(pending_airblow2 != cur2)
    //             {
    //                 User_Output_Set_Value(pending_airblow2 ? 1 : 0, (uint8_t)airblow2Index, handle);
    //             }

    //             if(!fault1)
    //             {
    //                 handle->Device[airblow1Index].state = pending_airblow1 ? DEVICE_STATE_ON : DEVICE_STATE_OFF;
    //             }
    //             if(!fault2)
    //             {
    //                 handle->Device[airblow2Index].state = pending_airblow2 ? DEVICE_STATE_ON : DEVICE_STATE_OFF;
    //             }

    //             AirBLow_1 = pending_airblow1 ? 1 : 0;
    //             AirBlow_2 = pending_airblow2 ? 1 : 0;
    //             airblow_pending = false;
    //         }

    //         both_fault_prev = both_fault;
    //     }
    //     else
    //     {
    //         AirBLow_1 = (handle->outputBuf & (1UL << airblow1Index)) ? 1 : 0;
    //         AirBlow_2 = (handle->outputBuf & (1UL << airblow2Index)) ? 1 : 0;
    //         airblow_pending = false;
    //     }
    // }
/////////////////////////////////////////////////////////////////////////////////////////////////////////////
}

void User_Output_Set_Value(uint8_t value, uint8_t deviceIndex, Device_Handle_t *handle)
{
    if(value == 1)
    {
        handle->outputBuf |= (1<<deviceIndex);
    }
    else
    {
        handle->outputBuf &= ~(1<<deviceIndex);
    }

    uint32_t mask;
    for(int i = 0; i < 32; i++)
    {
        mask = 0x80000000 >> i;
        if(handle->outputBuf & (mask))
        {
            gpio_set_level(IO_MOSI_PIN, 1);
        }
        else
        {
            gpio_set_level(IO_MOSI_PIN, 0);
        }

        IO_CLK_HIGH();
        IO_CLK_LOW();
    }

    IO_STR_HIGH();
    IO_STR_LOW();
}


void User_Out_Put_Flush_All(uint8_t state)
{
    gpio_set_level(IO_MOSI_PIN, state);

    for(int i = 0; i < 32; i++)
    {
        IO_CLK_HIGH();
        IO_CLK_LOW();
    }
    IO_STR_HIGH();
    IO_STR_LOW();

    for(int i = 0; i<DEVICE_MAX_NUM; i++)
    {
        DeviceHandle.Device[i].state = DEVICE_STATE_OFF;
    }
}



/* Functions for input */

bool User_Input_Check_Is_Active(gpio_num_t io)
{
    if(gpio_get_level(io) == IO_CONFIG_INPUT_ACTIVE)
    {
        return true;
    }
    else
    {
        return false;
    }
}

static void User_I2c_Check_Adress(void)
{
    esp_err_t err;
    for(uint8_t i = 3; i < 0x78; i++)
    {
        err = i2c_master_probe(i2cExHandle, i, 1000);
        if(err == ESP_OK)
        {
            isInputInit = true;
            ESP_LOGI("I2C SCAN: ", "Scan done, address: %02x", i);
            break;
        }
    }
}


/* Input functions area */

static void User_Input_Module_Init(void)
{
    i2c_master_bus_config_t i2c_mst_config =
    {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = TEST_I2C_PORT,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_mst_config, &i2cExHandle));

    i2c_device_config_t dev_cfg =
    {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x20,
        .scl_speed_hz = 100000,
    };

    ESP_ERROR_CHECK(i2c_master_bus_add_device(i2cExHandle, &dev_cfg, &i2cExDevHandle));

    User_I2c_Check_Adress();

    uint8_t dataConfig[2] = {0xFF, 0xFF};
    i2c_master_transmit(i2cExDevHandle, dataConfig, 2, 10);
}

/* Functions for communicating with expander device */
// static void User_Device(uint8_t *data, uint16_t size)
// {
//     i2c_master_transmit(i2cExDevHandle, data, size, 100);
// }

static esp_err_t User_Device_Get_Error_Status(uint8_t *data, uint16_t size)
{
    static uint8_t fail_count = 0;
    static TickType_t next_retry_tick = 0;

    if(!isInputInit)
    {
        return ESP_FAIL;
    }

    TickType_t now = xTaskGetTickCount();
    if(now < next_retry_tick)
    {
        return ESP_FAIL;
    }

    esp_err_t err = i2c_master_receive(i2cExDevHandle, data, size, 100);
    if(err == ESP_OK)
    {
        fail_count = 0;
        return ESP_OK;
    }

    fail_count++;
    if(fail_count >= 3)
    {
        ESP_LOGW(IO_INPUT_TAG, "I2C read failed, retry after 5s");
        next_retry_tick = now + pdMS_TO_TICKS(5000);
        fail_count = 0;
    }
    return err;

}

void IO_Driver_Task(void)
{
    outputBuf = 0xFFFFFFFF;
    User_Device_Init();
    User_Input_Module_Init();
    User_Out_Put_IO_Config();
    /* KHÔNG gọi User_Out_Put_Flush_All(0) ngay lập tức ở đây vì phần cứng (shift register)
     * vẫn đang giữ trạng thái cũ sau khi ESP reset. Ta sẽ đọc FRAM trước để quyết định. */

    /* ─── BƯỚC 1: Đọc FAULT states từ FRAM ─── */
    uint32_t saved_faults = 0xFFFFFFFF;
    Fram_Read_Data(0x1604, (uint8_t *)&saved_faults, sizeof(uint32_t));
    if(saved_faults != 0xFFFFFFFF)
    {
        device_fault_states = saved_faults;
        ESP_LOGI("OUTPUT", "Restored fault states from FRAM: 0x%08X", (unsigned int)saved_faults);
    }

    /* ─── BƯỚC 2: Đồng bộ cấu hình thiết bị từ FRAM slots vào RAM ─── */
    User_Device_Config_Sync_With_Fram(&DeviceHandle);

    /* ─── BƯỚC 3: Phục hồi outputBuf và trạng thái phần cứng ─── */
    uint32_t saved_outputBuf = 0xFFFFFFFF;
    Fram_Read_Data(0x1600, (uint8_t *)&saved_outputBuf, sizeof(uint32_t));

    if(saved_outputBuf == 0xFFFFFFFF)
    {
        /* Lần đầu boot hoặc FRAM trắng hoàn toàn: xóa phần cứng về 0 */
        User_Out_Put_Flush_All(0);
        ESP_LOGI("OUTPUT", "First boot / FRAM blank - all outputs cleared");
    }
    else if(saved_outputBuf != 0)
    {
        /* FRAM có trạng thái hợp lệ (khác 0): phục hồi phần cứng trực tiếp.
         * KHÔNG flush trước vì phần cứng đang giữ đúng trạng thái cũ. */
        DeviceHandle.outputBuf = saved_outputBuf;
        User_Output_Shift_Data(&DeviceHandle.outputBuf);
        ESP_LOGI("OUTPUT", "Restored output states from FRAM: 0x%08X", (unsigned int)saved_outputBuf);
    }
    else
    {
        /* saved_outputBuf == 0x00000000: FRAM bị ghi đè về 0 (có thể do lịch trình hết
         * hạn hoặc lỗi AirBlower trong lần chạy trước). Tái tạo từ device slot states
         * (chỉ khôi phục thiết bị chạy tay - không có lịch trình). */
        uint32_t reconstructed = 0;
        for(int i = 0; i < DEVICE_MAX_NUM; i++)
        {
            if(DeviceHandle.Device[i].isActived && DeviceHandle.Device[i].state == DEVICE_STATE_ON && !DeviceHandle.Device[i].isScheduled)
            {
                reconstructed |= (1UL << i);
            }
        }
        DeviceHandle.outputBuf = reconstructed;
        if(reconstructed != 0)
        {
            User_Output_Shift_Data(&DeviceHandle.outputBuf);
            ESP_LOGI("OUTPUT", "Reconstructed manual outputs from device slots: 0x%08X", (unsigned int)reconstructed);
        }
        else
        {
            /* Không có gì cần phục hồi: flush phần cứng về 0 cho khớp */
            User_Out_Put_Flush_All(0);
            ESP_LOGI("OUTPUT", "All outputs were OFF (FRAM=0) - hardware cleared");
        }
    }

    TickType_t xLastTelemetryTime = xTaskGetTickCount();

    /* Bảo vệ FRAM khỏi bị ghi đè trong 10s đầu tiên sau khi khởi động.
     * Trong giai đoạn này cảm biến I2C chưa ổn định, lịch trình cũ chưa
     * được xử lý đúng, tránh ghi đè trạng thái sai lên FRAM. */
    TickType_t startup_grace_ticks = xTaskGetTickCount() + pdMS_TO_TICKS(10000);
    ESP_LOGI("OUTPUT", "FRAM write protected for 10s during startup...");
    
    static TickType_t xLastBlinkTimeLED1 = 0;
    static uint8_t led1_state = 0;
    
    static TickType_t xLastBlinkTimeLED2 = 0;
    static uint8_t led2_state = 0;

    while(1)
    {
        if(Sys_Info.isRebootRequested && ((xTaskGetTickCount() - Sys_Info.rebootTimestamp) >= pdMS_TO_TICKS(5000)))
        {
            User_Device_Save_Runtimes_To_Fram(); // Lưu giờ chạy trước khi reboot
            ESP_LOGW("SYSTEM", "Auto-rebooting to apply configurations...");
            esp_restart();
        }

        // ─── KIỂM TRA MẤT KẾT NỐI WIFI QUÁ 5 PHÚT ───
        static TickType_t last_wifi_connected_tick = 0;
        static TickType_t last_warn_tick = 0;
        
        if(Sys_Info.isWifiConnected || User_Input_Check_Is_Active(IO_CONFIG_INPUT_1))
        {
            last_wifi_connected_tick = xTaskGetTickCount();
        }
        else
        {
            TickType_t diff = xTaskGetTickCount() - last_wifi_connected_tick;
            if(diff >= pdMS_TO_TICKS(300000)) // 5 phút
            {
                ESP_LOGE("SYSTEM", "WiFi disconnected for 5 minutes. Writing reset flag to FRAM and resetting ESP...");
                User_Device_Save_Runtimes_To_Fram(); // Lưu giờ chạy trước khi reset
                // Ghi cờ reset vào FRAM địa chỉ 0x1608 trước khi reset
                uint8_t flag = 1;
                Fram_Write_Data(0x1608, &flag, 1);
                esp_restart();
            }
            else if(xTaskGetTickCount() - last_warn_tick >= pdMS_TO_TICKS(60000)) // Cảnh báo mỗi 1 phút
            {
                last_warn_tick = xTaskGetTickCount();
                ESP_LOGW("SYSTEM", "WiFi disconnected for %d seconds. Will reset in %d seconds.", (int)(pdTICKS_TO_MS(diff) / 1000), (int)(300 - pdTICKS_TO_MS(diff) / 1000));
            }
        }

        // ─── KIỂM TRA AZURE KHÔNG KẾT NỐI LẠI ĐƯỢC SAU 5 PHÚT (WiFi đã có) ───
        static TickType_t last_azure_connected_tick = 0;
        static TickType_t last_azure_warn_tick = 0;
        if(IoTHubHandle.isAzureInitialized)
        {
            // Azure đang kết nối → cập nhật timestamp
            last_azure_connected_tick = xTaskGetTickCount();
        }
        else if(Sys_Info.isWifiConnected && last_azure_connected_tick != 0)
        {
            // WiFi có nhưng Azure không kết nối được
            TickType_t azure_diff = xTaskGetTickCount() - last_azure_connected_tick;
            if(azure_diff >= pdMS_TO_TICKS(300000)) // 5 phút
            {
                ESP_LOGE("SYSTEM", "Azure failed to reconnect for 5 min (WiFi is up). Restarting...");
                User_Device_Save_Runtimes_To_Fram(); // Lưu giờ chạy trước khi reset
                uint8_t flag = 2; // Code 2 = Azure watchdog reset
                Fram_Write_Data(0x1608, &flag, 1);
                esp_restart();
            }
            else if(xTaskGetTickCount() - last_azure_warn_tick >= pdMS_TO_TICKS(60000))
            {
                last_azure_warn_tick = xTaskGetTickCount();
                ESP_LOGW("SYSTEM", "Azure not connected for %d sec. Will reset in %d sec.", (int)(pdTICKS_TO_MS(azure_diff) / 1000), (int)(300 - pdTICKS_TO_MS(azure_diff) / 1000));
            }
        }
        else
        {
            // WiFi chưa có → không tính thời gian chờ Azure
            last_azure_connected_tick = xTaskGetTickCount();
        }

        // ─── KIỂM TRA VÀ GỬI BÁO CÁO TELEMETRY SAU KHI REBOOT DO MẤT MẠNG ───
        static bool reported_watchdog_reset = false;
        if(!reported_watchdog_reset && IoTHubHandle.isTransmitInitialized && Sys_Info.isWifiConnected && IoTHubHandle.isAzureInitialized)
        {
            uint8_t flag = 0;
            if(Fram_Read_Data(0x1608, &flag, 1) && flag != 0)
            {
                // Phân biệt nguyên nhân reset để báo cáo đúng
                int reset_code = 999;
                const char *reset_msg = "Unknown watchdog reset";
                if(flag == 1)
                {
                    reset_code = 999;
                    reset_msg = "Device rebooted: WiFi disconnected for 5+ minutes.";
                }
                else if(flag == 2)
                {
                    reset_code = 998;
                    reset_msg = "Device rebooted: Azure failed to reconnect for 5+ minutes (WiFi was up).";
                }

                cJSON *root = cJSON_CreateObject();
                cJSON *payload = cJSON_CreateObject();
                if(root != NULL && payload != NULL)
                {
                    cJSON_AddNumberToObject(root, "status", 200);
                    cJSON_AddItemToObject(root, "payload", payload);
                    cJSON_AddNumberToObject(payload, "Code", reset_code);
                    cJSON_AddNumberToObject(payload, "TimeStamp", (double)Sys_Info.epochtime);
                    cJSON_AddStringToObject(payload, "Message", reset_msg);

                    char *tele_str = cJSON_PrintUnformatted(root);
                    if(tele_str != NULL)
                    {
                        if(PushTelemetry(tele_str) == pdPASS)
                        {
                            // Xóa cờ trong FRAM sau khi đã đẩy vào hàng đợi thành công
                            uint8_t clear_flag = 0;
                            Fram_Write_Data(0x1608, &clear_flag, 1);
                            reported_watchdog_reset = true;
                            ESP_LOGI("SYSTEM", "Reported watchdog reset telemetry (code=%d)", reset_code);
                        }
                        free(tele_str);
                    }
                }
                if(root != NULL) cJSON_Delete(root);
            }
            else
            {
                reported_watchdog_reset = true; // Không có cờ hoặc đọc lỗi, bỏ qua
            }
        }

        User_Output_Polling(&DeviceHandle);

        // ─── ĐẾM THỜI GIAN CHẠY MỖI 1 GIÂY ───
        static TickType_t last_runtime_tick = 0;
        if (last_runtime_tick == 0)
        {
            last_runtime_tick = xTaskGetTickCount();
        }
        else if ((xTaskGetTickCount() - last_runtime_tick) >= pdMS_TO_TICKS(1000))
        {
            uint32_t elapsed = (xTaskGetTickCount() - last_runtime_tick) / pdMS_TO_TICKS(1000);
            last_runtime_tick = xTaskGetTickCount();
            
            for (int i = 0; i < DEVICE_MAX_NUM; i++)
            {
                if (DeviceHandle.Device[i].isActived && DeviceHandle.Device[i].state == DEVICE_STATE_ON)
                {
                    DeviceHandle.Device[i].runtime += elapsed;
                }
            }
        }



        /* Lưu lại trạng thái ra FRAM nếu có sự thay đổi (để phục hồi lại sau khi ngắt điện).
         * Chỉ bắt đầu ghi sau khi hết giai đoạn bảo vệ khởi động (10s) để tránh cảm biến
         * I2C chưa ổn định hoặc lịch trình hết hạn ghi đè mất trạng thái cũ. */
        bool startup_done = (xTaskGetTickCount() >= startup_grace_ticks);

        static uint32_t last_saved_outputBuf = 0xFFFFFFFF;
        if(last_saved_outputBuf == 0xFFFFFFFF)
        {
            last_saved_outputBuf = DeviceHandle.outputBuf;
        }
        if(startup_done && DeviceHandle.outputBuf != last_saved_outputBuf)
        {
            last_saved_outputBuf = DeviceHandle.outputBuf;
            Fram_Write_Data(0x1600, (uint8_t *)&DeviceHandle.outputBuf, sizeof(uint32_t));
            // Lưu ngay lập tức khi thiết bị bật/tắt để không mất dữ liệu chạy
            User_Device_Save_Runtimes_To_Fram();
        }

        // ─── TỰ ĐỘNG LƯU ĐỊNH KỲ 60 GIÂY VÀO FRAM ───
        static TickType_t last_runtime_save_tick = 0;
        if (last_runtime_save_tick == 0)
        {
            last_runtime_save_tick = xTaskGetTickCount();
        }
        else if (startup_done && (xTaskGetTickCount() - last_runtime_save_tick) >= pdMS_TO_TICKS(60000))
        {
            last_runtime_save_tick = xTaskGetTickCount();
            User_Device_Save_Runtimes_To_Fram();
        }

        static uint32_t last_saved_fault_states = 0xFFFFFFFF;
        if(last_saved_fault_states == 0xFFFFFFFF)
        {
            last_saved_fault_states = device_fault_states;
        }
        if(startup_done && device_fault_states != last_saved_fault_states)
        {
            last_saved_fault_states = device_fault_states;
            Fram_Write_Data(0x1604, (uint8_t *)&device_fault_states, sizeof(uint32_t));
        }

        TickType_t dynamicInterval = pdMS_TO_TICKS(Sys_Info.timeReportDevice > 0 ? (Sys_Info.timeReportDevice * 1000) : 10000);
        if((xTaskGetTickCount() - xLastTelemetryTime) >= dynamicInterval)
        {
            User_Device_Report();
            xLastTelemetryTime = xTaskGetTickCount();
        }




        // ==== LED 1 LOGIC ====
        if(Sys_Info.isWifiConnected && Sys_Info.isTimeSync && IoTHubHandle.isAzureInitialized)
        {
            // Trạng thái 1: Có kết nối WiFi, có internet, có kết nối Azure -> Nháy chu kỳ 100ms
            if((xTaskGetTickCount() - xLastBlinkTimeLED1) >= pdMS_TO_TICKS(100))
            {
                led1_state = !led1_state;
                gpio_set_level(LED_WIFI_PIN, led1_state);
                xLastBlinkTimeLED1 = xTaskGetTickCount();
            }
        }
        else if(Sys_Info.isWifiConnected && Sys_Info.isTimeSync && !IoTHubHandle.isAzureInitialized)
        {
            // Trạng thái 2: Có kết nối WiFi, có internet, không có Azure -> Nháy chu kỳ 1s
            if((xTaskGetTickCount() - xLastBlinkTimeLED1) >= pdMS_TO_TICKS(1000))
            {
                led1_state = !led1_state;
                gpio_set_level(LED_WIFI_PIN, led1_state);
                xLastBlinkTimeLED1 = xTaskGetTickCount();
            }
        }
        else if(Sys_Info.isWifiConnected && !Sys_Info.isTimeSync)
        {
            // Trạng thái 3: Có kết nối WiFi nhưng không có internet -> Luôn sáng
            gpio_set_level(LED_WIFI_PIN, 0);
        }
        else
        {
            // Trạng thái 4: Không có kết nối WiFi -> Đèn tắt
            gpio_set_level(LED_WIFI_PIN, 1);
        }

        // ==== LED 2 LOGIC ====
        if((xTaskGetTickCount() - Sys_Info.searchDeviceTimestamp) < pdMS_TO_TICKS(5000))
        {
            gpio_set_level(LED_AZURE_PIN, 0); // 5s Solid ON Search Mode
            xLastBlinkTimeLED2 = xTaskGetTickCount(); // sync blinker
        }
        else if(Sys_Info.isApActive)
        {
            uint32_t interval = Sys_Info.isApHasClient ? 100 : 1000;
            if((xTaskGetTickCount() - xLastBlinkTimeLED2) >= pdMS_TO_TICKS(interval))
            {
                led2_state = !led2_state;
                gpio_set_level(LED_AZURE_PIN, led2_state);
                xLastBlinkTimeLED2 = xTaskGetTickCount();
            }
        }
        else
        {
            gpio_set_level(LED_AZURE_PIN, 1);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        esp_task_wdt_reset(); // Lớp 4: Feed Task WDT — xác nhận IO Task vẫn đang chạy bình thường
    }
}
