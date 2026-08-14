#pragma once

#include "stdint.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "stdbool.h"
#include "time.h"

/* Define maximum number of deivce */
#define DEVICE_MAX_NUM  10
/* Define address */
#define FRAM_DEVICE_ADDR   0x0000
#define FRAM_DEVICE_SIZE   512 //sizeof(Device_Parameters_t)
/* Max schedules per device */
#define DEVICE_SCHEDULE_MAX 15
/* Define for input */
#define IO_CONFIG_INPUT_1   20
#define IO_CONFIG_INPUT_2   3

#define IO_CONFIG_INPUT_ACTIVE  0
#define IO_CONFIG_INPUT_DE_ACTIVE  1



/* Define for output */
#define SERVER_TRIGGER_IO_BIT   (1<<0)
#define IO_LED_EXTBOARD_PIN    41

#define LED_WIFI_PIN           19
#define LED_AZURE_PIN          8

#define GPIO_OUTPUT_PIN_SEL  ((1ULL<<IO_LED_EXTBOARD_PIN) | (1ULL<<IO_CLK_PIN) | (1ULL<<IO_MOSI_PIN) | (1ULL<<IO_STR_PIN) | (1ULL<<OUTPUT_ENABLE_PIN) | (1ULL<<LED_WIFI_PIN) | (1ULL<<LED_AZURE_PIN))

#define GPIO_INPUT_PIN_SEL  ((1ULL<<IO_CONFIG_INPUT_1) | (1ULL<<IO_CONFIG_INPUT_2))

#define IO_10_TEST      10


/* Define for load cell */

#define LOADCELL_SCK_OUTPUT_PIN 5
#define LOADCELL_DATA_INPUT_PIN 4

#define LOADCELL_OUTPUT_PIN_SEL  ((1ULL<<LOADCELL_SCK_OUTPUT_PIN))
#define LOADCELL_INPUT_PIN_SEL  ((1ULL<<LOADCELL_DATA_INPUT_PIN))


/* Define for input */

#define I2C_MASTER_SCL_IO   6
#define I2C_MASTER_SDA_IO   7
#define TEST_I2C_PORT       I2C_NUM_1


/* Define name and id for devices */
#define GROUP_1_DEVICE_NAME_1   "PaddleWheel_1"
#define GROUP_1_DEVICE_NAME_2   "PaddleWheel_2"
#define GROUP_1_DEVICE_NAME_3   "PaddleWheel_3"
#define GROUP_1_DEVICE_NAME_4   "PaddleWheel_4"

#define GROUP_1_DEVICE_ID_1   11
#define GROUP_1_DEVICE_ID_2   12
#define GROUP_1_DEVICE_ID_3   13
#define GROUP_1_DEVICE_ID_4   14


#define GROUP_2_DEVICE_NAME_1   "AirBlower_1"
#define GROUP_2_DEVICE_NAME_2   "AirBlower_2"


#define GROUP_2_DEVICE_ID_1   21
#define GROUP_2_DEVICE_ID_2   22


#define GROUP_3_DEVICE_NAME_1   "Feeder_1_M1"
#define GROUP_3_DEVICE_NAME_2   "Feeder_1_M2"


#define GROUP_3_DEVICE_ID_1   31
#define GROUP_3_DEVICE_ID_2   32

// #define GROUP_3_DEVICE_ID_1   32
// #define GROUP_3_DEVICE_ID_2   31


#define GROUP_4_DEVICE_NAME_1   "Syphon_1"
#define GROUP_4_DEVICE_NAME_2   "Syphon_2"


#define GROUP_4_DEVICE_ID_1   41
#define GROUP_4_DEVICE_ID_2   42




typedef enum
{
    DEVICE_STATE_OFF = 0,
    DEVICE_STATE_ON,
    DEVICE_STATE_FAULT
}Device_State_t;

typedef enum
{
    DEVICE_ACTIVE_TYPE_NONE =0,
    DEVICE_ACTIVE_TYPE_TRIGGER = 1,
    DEVICE_ACTIVE_TYPE_SCHEDULE = 2,
    DEVICE_ACTIVE_TYPE_CONFIG =3,
    DEVICE_ACTIVE_TYPE_UPDATE_FW = 4
}Device_ActiveType_t;

typedef struct __attribute__((packed))
{
    time_t startTime;
    time_t stopTime;
    uint8_t isFinished;
}Device_Schedule_t;

typedef struct __attribute__((packed))
{
    bool isActived;
    bool isScheduled;
    bool isSchedulePaused;   // true = lịch trình bị tạm dừng để điều khiển thủ công (CMD 111)

    time_t startTime;
    time_t stopTime;
    uint32_t duration;

    TickType_t stopAtTick;
    uint16_t id;
    Device_State_t state;
    uint8_t index;
    uint32_t weight;
    char name[32];
    uint32_t runtime;        // Daily runtime in seconds
    uint8_t scheduleCount;
    uint8_t scheduleIndex;
    Device_Schedule_t schedules[DEVICE_SCHEDULE_MAX];
}Device_Parameters_t;

typedef struct 
{
    bool isInputChecked;
    bool isTriggered;
    bool isNeedOnTime;
    uint16_t inputBuf;
    uint32_t outputBuf;
    Device_ActiveType_t activeType;
    Device_Parameters_t Device[DEVICE_MAX_NUM];
}Device_Handle_t;

typedef struct __attribute__((packed))
{
    uint16_t id;
    uint8_t index;
    uint8_t isScheduled;
    uint8_t isSchedulePaused;   // 1 = lịch trình đang bị tạm dừng (lưu vào FRAM)
    Device_State_t state;
    time_t startTime;
    time_t stopTime;
    uint8_t scheduleCount;
    uint8_t scheduleIndex;
    Device_Schedule_t schedules[DEVICE_SCHEDULE_MAX];
}Device_Persist_t;

typedef struct 
{
    bool isConfigMode;
}IO_Input_Config_Handle_t;

extern Device_Handle_t DeviceHandle;
extern IO_Input_Config_Handle_t InputConfHandle;
extern EventGroupHandle_t IO_Event_Group;

extern bool isManualFeeder_M2;

void User_Out_Put_IO_Config(void);

void User_Input_IO_Config(void);

bool User_Input_Check_Is_Active(gpio_num_t io);

void User_Output_Set_Value(uint8_t value, uint8_t deviceIndex, Device_Handle_t *handle);

void User_Out_Put_Flush_All(uint8_t state);

void User_Output_Shift_Data(const uint32_t *data);

void User_Output_Parse_Buffer(Device_Handle_t *handle);

void User_Output_Deploy(Device_Handle_t *handle);

void IO_Driver_Task(void);

void User_Feeder_M2_Manual_Control(bool on);

void User_Device_Save_Runtimes_To_Fram(void);
void User_Device_Report_Daily_Runtimes_And_Reset(void);
void User_Device_Ack_Daily_Runtimes(void);

extern double vfd_last_energy;
extern double vfd_daily_energy;

void Update_Vfd_Energy(double current_cumulative);

void User_Device_Init(void);
void User_Device_Report(void);

/**
 * @brief Chuyển đổi chế độ ao (Pond Mode) và đồng bộ lại FRAM + RAM.
 *        Xóa N slot FRAM cũ rồi ghi pondMode mới, sau đó reboot.
 * @param new_mode POND_MODE_10_DEV hoặc POND_MODE_4_DEV (từ Pond_Mode_t)
 */
void User_Device_Switch_Pond_Mode(uint8_t new_mode);
