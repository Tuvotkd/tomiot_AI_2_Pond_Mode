#pragma once

#include "tcp_server_com.h"

#include "stdint.h"
#include "stdbool.h"
#include "time.h"

// #define CRC32_BYPASS        1

#define START_OF_FRAME 0x02
#define END_OF_FRAME    0x03

#define SYS_SERVER_IP_DEFAULT   "192.168.1.100"
#define SYS_SERVER_PORT_DEFAULT   3000
#define SYS_WIFI_SSID_DEFAULT   "MebiOneIOT"
#define SYS_WIFI_PASS_DEFAULT   "MebiOne@123"


#define SYS_IOT_HUB_HOST_NAME_DEFAULT           "VuIoTHub.azure-devices.net"
#define SYS_IOT_HUB_DEVICE_ID_DEFAULT           "VuDevice"
#define SYS_IOT_HUB_SYMMETRIC_KEY_DEFAULT       "xOw6BICGo79moY00yntL7ZTQ0eRZD3sICfOe+29qRr0="

#define DEVICE_ID_GROUP_1_NUM1  11
#define DEVICE_ID_GROUP_1_NUM2  12
#define DEVICE_ID_GROUP_1_NUM3  13
#define DEVICE_ID_GROUP_1_NUM4  14

#define DEVICE_ID_GROUP_2_NUM1  21
#define DEVICE_ID_GROUP_2_NUM2  22
#define DEVICE_ID_GROUP_2_NUM3  23
#define DEVICE_ID_GROUP_2_NUM4  24

#define DEVICE_ID_GROUP_3_NUM1  31
#define DEVICE_ID_GROUP_3_NUM2  32
#define DEVICE_ID_GROUP_3_NUM3  33
#define DEVICE_ID_GROUP_3_NUM4  34

#define DEVICE_ID_GROUP_4_NUM1  41
#define DEVICE_ID_GROUP_4_NUM2  42
#define DEVICE_ID_GROUP_4_NUM3  43
#define DEVICE_ID_GROUP_4_NUM4  44

/* Define for FRAM */
#define DEVICE_CONFIG_START_ADDRESS 0x1000
#define DEVICE_SPACE_LEN_IN_FRAM    512
#define NUM_OF_DEVICE               12

#define FRAM_FEEDER_MODE_ADDR       0x160A // 1 byte
#define FRAM_FEEDER_ACTIVE_ID_ADDR  0x160B // 2 bytes

#define FRAM_OXY_MODE_ADDR          0x160C // 1 byte
#define FRAM_OXY_ACTIVE_ID_ADDR     0x160D // 2 bytes

#define FRAM_POND_MODE_ADDR         0x1610 // 1 byte: 0 = Mode 10 thiết bị, 1 = Mode 4 thiết bị
#define FRAM_VFD_ENABLED_ADDR       0x1611 // 1 byte: 0 = Vô hiệu hóa, 1 = Kích hoạt (mặc định)
#define FRAM_PERF_MONITOR_ENABLED_ADDR 0x1612 // 1 byte: 0 = Tắt (mặc định), 1 = Bật
#define FRAM_CONSOLE_MONITOR_ENABLED_ADDR 0x1613 // 1 byte: 0 = Tắt (mặc định), 1 = Bật
#define FRAM_IP_ADDR_ADDR           0x1620 // 16 bytes: IP address string format (e.g., "192.168.1.100")

/**
 * @brief Chế độ ao nuôi: 10 thiết bị đầy đủ hoặc 4 thiết bị cơ bản
 */
typedef enum {
    POND_MODE_10_DEV = 0,   // 10 thiết bị: 11,12,13,14,21,22,31,32,41,42
    POND_MODE_4_DEV  = 1,   // 4 thiết bị:  11,12,21,41
} Pond_Mode_t;

/* Define cmd code */
#define CMD_CODE_CONTROL_ON_OFF             101
#define CMD_CODE_CONTROL_SCHEDULE           102
#define CMD_CODE_MODIFY_CONTROL_SCHEDULE    103
#define CMD_CODE_DELETE_SCHEDULE            104
#define CMD_CODE_SAVE_SCHEDULE_AND_EXECUTE  105
#define CMD_CODE_SET_WIFI_ONLINE            106
#define CMD_CODE_SET_AZURE_ONLINE           107
#define CMD_CODE_SET_TIME_OF_DEVICE_REPORT  108
#define CMD_CODE_CLEAR_FRAM                 109
#define CMD_CODE_RESET_ESP                  110
#define CMD_CODE_IDENTIFY_PAUSE_SCHEDULE    111
#define CMD_CODE_IDENTIFY_RESUME_SCHEDULE   112
#define CMD_CODE_SET_TIME_OF_VFD_REPORT     113
#define CMD_CODE_OXY_POND                   114
#define CMD_CODE_FEEDER_CONTACTOR_POND      115

#define CMD_CODE_UPDATE_FIRMWARE            501
#define CMD_CODE_ASK_VERSION                502
#define CMD_CODE_ASK_RESETCOUNT             503
#define CMD_CODE_ASK_RUNNING_TIME           504
#define CMD_CODE_ASK_IP_WEB_PORTAL          505

#define CMD_CODE_RUN_FORWARD               200
#define CMD_CODE_RUN_REVERSE               201
#define CMD_CODE_STOP                      202
#define CMD_CODE_SET_SOFT_START_TIME       203
#define CMD_CODE_SET_SOFT_STOP_TIME        204
#define CMD_CODE_SET_FREQUENCE             205
#define CMD_CODE_ENABLE_OPTIMIZE_VFD       206
#define CMD_CODE_DISABLE_OPTIMIZE_VFD      207
#define CMD_CODE_CONTROL_VFD_MANUAL        208
#define CMD_CODE_CONTROL_VFD_AUTO          209
#define CMD_CODE_SET_FUNCTION_CODE         210
#define CMD_CODE_AUTO_RUN_AFTER_POWER_BACK 211



/* VERSION được lấy tự động từ PROJECT_VER trong CMakeLists.txt
   Dùng esp_app_get_description()->version tại runtime */


/* Define Telemetry queue length */
#define TELEMETRY_QUEUE_LENGTH  20

typedef struct 
{
    /* data */
    bool isWifiConnected;
    bool isTimeSync;

    bool isTimeSyncCb;
    time_t epochtime;

    bool isApActive;
    bool isApHasClient;
    uint32_t searchDeviceTimestamp;

    bool isRebootRequested;
    uint32_t rebootTimestamp;
    uint32_t timeReportDevice;

    uint8_t feederMode;        // 0: 2 động cơ, 1: 1 động cơ
    uint16_t activeFeederId;   // 31, 32 hoặc 99 (khi feederMode = 0)
    
    uint8_t oxyMode;           // 0: 2 máy oxy, 1: 1 máy oxy
    uint16_t activeOxyId;      // 21, 22 hoặc 99 (khi oxyMode = 0)

    uint8_t pondMode;          // Pond_Mode_t: 0 = 10 thiết bị, 1 = 4 thiết bị
    uint8_t vfdEnabled;        // 0: Disabled, 1: Enabled
    uint8_t perfMonitorEnabled; // 0: Tắt (mặc định), 1: Bật
    uint8_t consoleMonitorEnabled; // 0: Tắt (mặc định), 1: Bật
}Sys_Info_Handle_t;

extern Sys_Info_Handle_t Sys_Info;
extern uint32_t reset_count;

void User_System_Get_Config(void);

void User_System_Init(void);

bool Is_System_Time_Synchronized(void);

bool Is_System_Internet_Connected(void);

void User_System_Clear_Reset_Count(void);

void User_System_Search_Device(void);
