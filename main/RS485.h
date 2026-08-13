/**
 * @file    RS485.h
 * @brief   Thư viện điều khiển biến tần INVT GD200A qua giao thức Modbus RTU / RS485
 * @version 1.0.0
 * @date    2025
 *
 * @details
 *  Hỗ trợ ESP-IDF v5.x
 *  Mẫu biến tần: GD200A-011G/015P-4 (11kW/15kW, 3 pha 380V)
 *
 *  Địa chỉ thanh ghi tham chiếu từ GD200A Manual V2.8 - Chương 9 (Modbus Protocol):
 *  - 0x2000  : Communication Control Command (Write) – Lệnh điều khiển
 *  - 0x2001  : Communication Setting Frequency (R/W) – Cài đặt tần số (unit: 0.01Hz)
 *  - 0x2100  : VFD Status Word 1 (Read) – Trạng thái vận hành
 *  - 0x2101  : VFD Status Word 2 (Read) – Trạng thái mở rộng
 *  - 0x2102  : Fault Code (Read) – Mã lỗi
 *  - 0x3000  : Operation Frequency (Read) – Tần số thực tế (unit: 0.01Hz)
 *  - 0x3001  : Setting Frequency (Read) – Tần số đặt (unit: 0.01Hz)
 *  - 0x3002  : Bus Voltage (Read) – Điện áp DC Bus (unit: V)
 *  - 0x3003  : Output Voltage (Read) – Điện áp ngõ ra (unit: V)
 *  - 0x3004  : Output Current (Read) – Dòng điện ngõ ra (unit: 0.1A)
 *  - 0x3005  : Operation Speed (Read) – Tốc độ động cơ (unit: RPM)
 *  - 0x3006  : Output Power (Read) – Công suất ngõ ra (unit: 0.1%)
 *  - 0x5000  : Fault Code detail (Read)
 *
 *  Địa chỉ thanh ghi cài đặt Soft Start/Stop (P00.11 / P00.12):
 *  - 0x000B  : P00.11 – Acceleration Time 1 (unit: 0.1s, range: 0–36000 → 0.0–3600.0s)
 *  - 0x000C  : P00.12 – Deceleration Time 1 (unit: 0.1s, range: 0–36000 → 0.0–3600.0s)
 *
 *  Thiết lập bắt buộc trên biến tần trước khi dùng thư viện này:
 *  - P00.01 = 2  (Run command channel: MODBUS communication)
 *  - P00.02 = 0  (Communication selection: MODBUS)
 *  - P00.06 = 8  (Frequency source A: MODBUS communication)
 *  - P14.00 = 1  (Baud rate: 9600bps, hoặc chỉnh theo P14.00)
 *  - P14.01 = 1  (Data format: 8N1)
 *  - P14.02 = 1  (Slave address: 0x01)
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* ============================================================
 * UART & GPIO Configuration – Chỉnh theo phần cứng của bạn
 * ============================================================ */
#define RS485_UART_PORT         UART_NUM_2
#define RS485_UART_BAUD_RATE    19200
#define RS485_GPIO_TX           38
#define RS485_GPIO_RX           40
#define RS485_GPIO_DIR          39       ///< GPIO điều khiển hướng RS485 (DE/RE): HIGH = TX, LOW = RX
                                        ///< Nếu không dùng, đặt = -1
#define RS485_UART_BUF_SIZE     256
#define RS485_RESPONSE_TIMEOUT_MS  100  ///< Thời gian chờ phản hồi từ biến tần (ms)
#define RS485_RETRY_COUNT       3       ///< Số lần thử lại khi giao tiếp thất bại

/* ============================================================
 * Modbus Slave Address của biến tần (P14.02)
 * ============================================================ */
#define GD200A_SLAVE_ADDR       0x01

/* ============================================================
 * Modbus Function Codes
 * ============================================================ */
#define MODBUS_FC_READ_HOLDING      0x03    ///< Đọc nhiều thanh ghi (Read N Words)
#define MODBUS_FC_WRITE_SINGLE      0x06    ///< Ghi 1 thanh ghi (Write Single Word)
#define MODBUS_FC_WRITE_MULTIPLE    0x10    ///< Ghi nhiều thanh ghi liên tiếp

/* ============================================================
 * Địa chỉ thanh ghi điều khiển (Ref: GD200A Manual Ch.9, Table 9.5.2)
 * ============================================================ */

/* Control & Setting Registers (R/W) */
#define GD200A_REG_CTRL_CMD         0x2000  ///< Lệnh điều khiển vận hành
#define GD200A_REG_FREQ_SET         0x2001  ///< Tần số đặt qua truyền thông (unit: 0.01Hz)
#define GD200A_REG_PID_REF          0x2002  ///< PID reference
#define GD200A_REG_PID_FB           0x2003  ///< PID feedback
#define GD200A_REG_TORQUE_SET       0x2004  ///< Giá trị mô-men đặt
#define GD200A_REG_FREQ_UPPER_FWD   0x2005  ///< Giới hạn trên tần số chiều thuận
#define GD200A_REG_FREQ_UPPER_REV   0x2006  ///< Giới hạn trên tần số chiều nghịch

/* Status Registers (Read Only) */
#define GD200A_REG_STATUS_1         0x2100  ///< Từ trạng thái 1
#define GD200A_REG_STATUS_2         0x2101  ///< Từ trạng thái 2
#define GD200A_REG_FAULT_CODE_COMM  0x2102  ///< Mã lỗi truyền thông
#define GD200A_REG_DEVICE_CODE      0x2103  ///< Mã nhận dạng thiết bị (GD200A = 0x0107)

/* Monitoring Registers (Read Only) */
#define GD200A_REG_FREQ_ACT         0x3000  ///< Tần số vận hành thực tế (unit: 0.01Hz)
#define GD200A_REG_FREQ_REF         0x3001  ///< Tần số đặt hiện tại (unit: 0.01Hz)
#define GD200A_REG_BUS_VOLTAGE      0x3002  ///< Điện áp DC Bus (unit: V)
#define GD200A_REG_OUTPUT_VOLTAGE   0x3003  ///< Điện áp ngõ ra (unit: V)
#define GD200A_REG_OUTPUT_CURRENT   0x3004  ///< Dòng điện ngõ ra (unit: 0.1A)
#define GD200A_REG_MOTOR_SPEED      0x3005  ///< Tốc độ động cơ (unit: RPM)
#define GD200A_REG_OUTPUT_POWER     0x3006  ///< Công suất ngõ ra (unit: 0.1%)
#define GD200A_REG_OUTPUT_TORQUE    0x3007  ///< Mô-men ngõ ra

/* Fault Register */
#define GD200A_REG_FAULT_CODE       0x5000  ///< Mã lỗi chi tiết

/* Function Code Parameter Registers */
#define GD200A_REG_RUN_CMD_CH       0x0001  ///< P00.01 – Kênh điều khiển vận hành
#define GD200A_REG_COMM_SEL         0x0002  ///< P00.02 – Lựa chọn truyền thông
#define GD200A_REG_MAX_FREQ         0x0003  ///< P00.03 – Tần số cực đại (unit: 0.01Hz)
#define GD200A_REG_ACC_TIME_1       0x000B  ///< P00.11 – Thời gian tăng tốc 1 (unit: 0.1s)
#define GD200A_REG_DEC_TIME_1       0x000C  ///< P00.12 – Thời gian giảm tốc 1 (unit: 0.1s)

/* ============================================================
 * Control Command Values (ghi vào GD200A_REG_CTRL_CMD 0x2000)
 * ============================================================ */
#define GD200A_CMD_FORWARD_RUN      0x0001  ///< Chạy thuận
#define GD200A_CMD_REVERSE_RUN      0x0002  ///< Chạy nghịch
#define GD200A_CMD_FWD_JOG          0x0003  ///< Jog thuận
#define GD200A_CMD_REV_JOG          0x0004  ///< Jog nghịch
#define GD200A_CMD_STOP             0x0005  ///< Dừng (theo cấu hình dừng)
#define GD200A_CMD_COAST_STOP       0x0006  ///< Dừng tự do (coast to stop)
#define GD200A_CMD_FAULT_RESET      0x0007  ///< Reset lỗi
#define GD200A_CMD_JOG_STOP         0x0008  ///< Dừng jog

/* ============================================================
 * Status Word 1 (GD200A_REG_STATUS_1 0x2100) – Bit definition
 * ============================================================ */
#define GD200A_STATUS_FORWARD       0x0001  ///< Đang chạy thuận
#define GD200A_STATUS_REVERSE       0x0002  ///< Đang chạy nghịch
#define GD200A_STATUS_STOP          0x0003  ///< Đang dừng
#define GD200A_STATUS_FAULT         0x0004  ///< Lỗi
#define GD200A_STATUS_POFF          0x0005  ///< POFF state

/* ============================================================
 * Fault Code Definitions (GD200A_REG_FAULT_CODE_COMM 0x2102)
 * ============================================================ */
typedef enum
{
    GD200A_FAULT_NONE       = 0x00,
    GD200A_FAULT_OVERCUR_ACC= 0x01,    ///< Quá dòng khi tăng tốc
    GD200A_FAULT_OVERCUR_DEC= 0x02,    ///< Quá dòng khi giảm tốc
    GD200A_FAULT_OVERCUR_CST= 0x03,    ///< Quá dòng ở tốc độ ổn định
    GD200A_FAULT_OVERVOLT_ACC= 0x04,   ///< Quá áp khi tăng tốc
    GD200A_FAULT_OVERVOLT_DEC= 0x05,   ///< Quá áp khi giảm tốc
    GD200A_FAULT_OVERVOLT_CST= 0x06,   ///< Quá áp ở tốc độ ổn định
    GD200A_FAULT_UNDERVOLT  = 0x07,    ///< Thiếu điện áp DC Bus
    GD200A_FAULT_OVERLOAD   = 0x09,    ///< Quá tải biến tần
    GD200A_FAULT_MOTOR_OVL  = 0x0A,   ///< Quá tải động cơ
    GD200A_FAULT_PHASE_LOSS = 0x15,    ///< Mất pha đầu vào
    GD200A_FAULT_OVERHEAT   = 0x18,   ///< Quá nhiệt biến tần
    GD200A_FAULT_COMM_ERR   = 0x28,   ///< Lỗi truyền thông Modbus
} GD200A_FaultCode_t;

/* ============================================================
 * Kiểu dữ liệu kết quả trả về
 * ============================================================ */
typedef enum
{
    RS485_OK            =  0,    ///< Thành công
    RS485_ERR_TIMEOUT   = -1,    ///< Không nhận được phản hồi
    RS485_ERR_CRC       = -2,    ///< CRC không khớp
    RS485_ERR_EXCEPTION = -3,    ///< Biến tần trả về mã lỗi exception
    RS485_ERR_PARAM     = -4,    ///< Tham số truyền vào không hợp lệ
    RS485_ERR_NO_INIT   = -5,    ///< Chưa khởi tạo RS485
} RS485_Status_t;

/* ============================================================
 * Cấu trúc lưu trữ thông số vận hành của biến tần
 * ============================================================ */
typedef struct
{
    double  freq_actual_hz;     ///< Tần số thực tế (Hz)
    double  freq_set_hz;        ///< Tần số đặt (Hz)
    double  accel_time_s;       ///< Thời gian tăng tốc 1 (s)
    double  decel_time_s;       ///< Thời gian giảm tốc 1 (s)
    double  output_current_a;   ///< Dòng điện ngõ ra (A)
    uint16_t output_voltage_v;  ///< Điện áp ngõ ra (V)
    double  bus_voltage_v;     ///< Điện áp DC Bus (V)
    uint16_t motor_speed_rpm;   ///< Tốc độ động cơ (RPM)
    double  output_power_pct;   ///< Công suất ngõ ra (%)
    uint16_t status_word;       ///< Từ trạng thái 1 (raw)
    uint16_t fault_code;        ///< Mã lỗi
    bool    is_running;         ///< TRUE nếu đang chạy
    bool    is_fwd;             ///< TRUE nếu đang chạy chiều thuận
    bool    is_fault;           ///< TRUE nếu đang có lỗi
    uint16_t run_command_channel; ///< Kênh lệnh chạy (P00.01)
    uint16_t auto_run_enable;   ///< P01.21: 0 = Vô hiệu hóa, 1 = Kích hoạt
    double   auto_run_delay_s;  ///< P01.22: Thời gian trễ tự khởi động lại (s)
    double   cumulative_energy_kwh; ///< Điện năng tiêu thụ tích luỹ (kWh)
} GD200A_Status_t;

/* ============================================================
 * API PUBLIC – Khởi tạo
 * ============================================================ */

/**
 * @brief  Khởi tạo UART và cấu hình driver RS485 cho ESP-IDF
 * @return RS485_OK nếu thành công
 */
RS485_Status_t RS485_Init(void);

/**
 * @brief  Giải phóng driver UART (gọi khi không cần dùng nữa)
 */
void RS485_Deinit(void);

/* ============================================================
 * API PUBLIC – Điều khiển vận hành
 * ============================================================ */

/**
 * @brief  Chạy thuận (Forward Run)
 * @param  slave_addr  Địa chỉ slave của biến tần (thường = GD200A_SLAVE_ADDR)
 * @return RS485_OK nếu thành công
 */
RS485_Status_t GD200A_RunForward(uint8_t slave_addr);

/**
 * @brief  Chạy nghịch (Reverse Run)
 * @param  slave_addr  Địa chỉ slave của biến tần
 * @return RS485_OK nếu thành công
 */
RS485_Status_t GD200A_RunReverse(uint8_t slave_addr);

/**
 * @brief  Dừng theo lịch giảm tốc (Soft Stop / Ramp Stop)
 * @param  slave_addr  Địa chỉ slave của biến tần
 * @return RS485_OK nếu thành công
 */
RS485_Status_t GD200A_Stop(uint8_t slave_addr);

/**
 * @brief  Dừng tự do (Coast to Stop / Free-wheel Stop)
 * @param  slave_addr  Địa chỉ slave của biến tần
 * @return RS485_OK nếu thành công
 */
RS485_Status_t GD200A_CoastStop(uint8_t slave_addr);

/**
 * @brief  Reset lỗi biến tần
 * @param  slave_addr  Địa chỉ slave của biến tần
 * @return RS485_OK nếu thành công
 */
RS485_Status_t GD200A_FaultReset(uint8_t slave_addr);

/* ============================================================
 * API PUBLIC – Cài đặt tần số
 * ============================================================ */

/**
 * @brief  Cài đặt tần số vận hành qua truyền thông
 * @param  slave_addr  Địa chỉ slave của biến tần
 * @param  freq_hz     Tần số đặt (Hz), ví dụ: 50.0 → 50Hz
 *                     Giá trị hợp lệ: 0.00Hz đến P00.03 (max freq)
 * @return RS485_OK nếu thành công
 */
RS485_Status_t GD200A_SetFrequency(uint8_t slave_addr, float freq_hz);

/* ============================================================
 * API PUBLIC – Soft Start / Soft Stop (cài đặt thời gian tăng/giảm tốc)
 * ============================================================ */

/**
 * @brief  Cài đặt thời gian Soft Start (Acceleration Time 1 – P00.11)
 *         Đây là thời gian để biến tần tăng từ 0Hz đến tần số tối đa (P00.03)
 * @param  slave_addr  Địa chỉ slave của biến tần
 * @param  time_s      Thời gian tăng tốc (giây), ví dụ: 5.0 → 5 giây
 *                     Giá trị hợp lệ: 0.0s đến 3600.0s
 * @return RS485_OK nếu thành công
 * @note   Ghi vào RAM (không lưu EEPROM để bảo vệ bộ nhớ)
 */
RS485_Status_t GD200A_SetAccelTime(uint8_t slave_addr, float time_s);

/**
 * @brief  Cài đặt thời gian Soft Stop (Deceleration Time 1 – P00.12)
 *         Đây là thời gian để biến tần giảm từ tần số tối đa (P00.03) xuống 0Hz
 * @param  slave_addr  Địa chỉ slave của biến tần
 * @param  time_s      Thời gian giảm tốc (giây), ví dụ: 10.0 → 10 giây
 *                     Giá trị hợp lệ: 0.0s đến 3600.0s
 * @return RS485_OK nếu thành công
 * @note   Ghi vào RAM (không lưu EEPROM để bảo vệ bộ nhớ)
 */
RS485_Status_t GD200A_SetDecelTime(uint8_t slave_addr, float time_s);

/**
 * @brief  Cài đặt đồng thời cả Accel và Decel time
 * @param  slave_addr    Địa chỉ slave của biến tần
 * @param  accel_time_s  Thời gian tăng tốc (giây)
 * @param  decel_time_s  Thời gian giảm tốc (giây)
 * @return RS485_OK nếu thành công
 */
RS485_Status_t GD200A_SetSoftStartStop(uint8_t slave_addr, float accel_time_s, float decel_time_s);

/**
 * @brief  Cấu hình tự khởi động lại sau khi có điện (P01.21 và P01.22)
 * @param  slave_addr  Địa chỉ slave của biến tần
 * @param  enable      0 = Vô hiệu hóa, 1 = Kích hoạt
 * @param  delay_s     Thời gian trễ tính bằng giây (0.0s đến 3600.0s)
 * @return RS485_OK nếu thành công
 */
RS485_Status_t GD200A_SetAutoRunAfterPowerBack(uint8_t slave_addr, uint8_t enable, float delay_s);

/* ============================================================
 * API PUBLIC – Đọc thông số vận hành
 * ============================================================ */

/**
 * @brief  Đọc toàn bộ thông số vận hành của biến tần (1 lần giao tiếp)
 *         Đọc khối 7 thanh ghi liên tiếp từ 0x3000 đến 0x3006
 * @param  slave_addr  Địa chỉ slave của biến tần
 * @param  status      Con trỏ đến cấu trúc nhận kết quả
 * @return RS485_OK nếu thành công
 */
RS485_Status_t GD200A_ReadStatus(uint8_t slave_addr, GD200A_Status_t *status);

/**
 * @brief  Đọc tần số vận hành thực tế
 * @param  slave_addr  Địa chỉ slave của biến tần
 * @param  freq_hz     Con trỏ nhận giá trị tần số (Hz)
 * @return RS485_OK nếu thành công
 */
RS485_Status_t GD200A_ReadFrequency(uint8_t slave_addr, float *freq_hz);

/**
 * @brief  Đọc dòng điện ngõ ra
 * @param  slave_addr  Địa chỉ slave của biến tần
 * @param  current_a   Con trỏ nhận giá trị dòng điện (A)
 * @return RS485_OK nếu thành công
 */
RS485_Status_t GD200A_ReadCurrent(uint8_t slave_addr, float *current_a);

/**
 * @brief  Đọc điện áp ngõ ra
 * @param  slave_addr   Địa chỉ slave của biến tần
 * @param  voltage_v    Con trỏ nhận giá trị điện áp ngõ ra (V)
 * @return RS485_OK nếu thành công
 */
RS485_Status_t GD200A_ReadOutputVoltage(uint8_t slave_addr, uint16_t *voltage_v);

/**
 * @brief  Đọc mã lỗi hiện tại của biến tần
 * @param  slave_addr  Địa chỉ slave của biến tần
 * @param  fault_code  Con trỏ nhận mã lỗi (xem enum GD200A_FaultCode_t)
 * @return RS485_OK nếu thành công
 */
RS485_Status_t GD200A_ReadFaultCode(uint8_t slave_addr, uint16_t *fault_code);

/**
 * @brief  Lấy chuỗi mô tả mã lỗi bằng tiếng Anh
 * @param  fault_code  Mã lỗi
 * @return Chuỗi mô tả ngắn gọn
 */
const char* GD200A_GetFaultString(uint16_t fault_code);

/* ============================================================
 * API PUBLIC – Modbus low-level (nâng cao)
 * ============================================================ */

/**
 * @brief  Đọc N thanh ghi liên tiếp từ biến tần (Modbus FC03)
 * @param  slave_addr    Địa chỉ slave
 * @param  reg_addr      Địa chỉ thanh ghi bắt đầu
 * @param  reg_count     Số lượng thanh ghi cần đọc (tối đa 16)
 * @param  out_data      Mảng uint16_t nhận dữ liệu
 * @return RS485_OK nếu thành công
 */
RS485_Status_t Modbus_ReadRegisters(uint8_t slave_addr, uint16_t reg_addr,  uint8_t reg_count, uint16_t *out_data);

/**
 * @brief  Ghi 1 thanh ghi vào biến tần (Modbus FC06)
 * @param  slave_addr    Địa chỉ slave
 * @param  reg_addr      Địa chỉ thanh ghi
 * @param  value         Giá trị cần ghi (uint16_t)
 * @return RS485_OK nếu thành công
 */
RS485_Status_t Modbus_WriteSingleReg(uint8_t slave_addr, uint16_t reg_addr, uint16_t value);

/**
 * @brief  Ghi nhiều thanh ghi liên tiếp (Modbus FC10)
 * @param  slave_addr    Địa chỉ slave
 * @param  reg_addr      Địa chỉ thanh ghi bắt đầu
 * @param  reg_count     Số thanh ghi cần ghi (tối đa 16)
 * @param  data          Mảng dữ liệu cần ghi
 * @return RS485_OK nếu thành công
 */
RS485_Status_t Modbus_WriteMultipleRegs(uint8_t slave_addr, uint16_t reg_addr, uint8_t reg_count, uint16_t *data);

/**
 * @brief  Kích hoạt tối ưu hóa biến tần cho ao nuôi tôm (Ghi vào EEPROM)
 *         1. Tự khởi động lại sau khi có điện (P01.21 = 1, P01.22 = 3.0s)
 *         2. Tự động reset lỗi thoáng qua (P08.28 = 5 lần, P08.29 = 2.0s)
 *         3. Ổn định điện áp ngõ ra (AVR) (P00.16 = 1)
 *         4. Đường cong V/F quạt/bơm tiết kiệm điện (P04.00 = 4, P04.26 = 1)
 *         5. Bảo vệ quá tải động cơ thường (P02.26 = 1, P02.27 = 100.0%)
 * @param  slave_addr  Địa chỉ slave của biến tần
 * @return RS485_OK nếu toàn bộ 9 tham số được cài đặt thành công
 */
RS485_Status_t GD200A_EnableOptimizeForShrimpFarm(uint8_t slave_addr);

/**
 * @brief  Tắt cấu hình tối ưu hóa biến tần ao tôm, trả về mặc định nhà máy (Ghi vào EEPROM)
 *         1. Tắt tự khởi động lại sau khi có điện (P01.21 = 0, P01.22 = 1.0s)
 *         2. Tắt tự động reset lỗi thoáng qua (P08.28 = 0, P08.29 = 1.0s)
 *         3. Ổn định điện áp ngõ ra (AVR) (P00.16 = 1)
 *         4. Đường cong V/F tuyến tính (P04.00 = 0, P04.26 = 0)
 *         5. Bảo vệ quá tải động cơ biến tần (P02.26 = 2, P02.27 = 100.0%)
 * @param  slave_addr  Địa chỉ slave của biến tần
 * @return RS485_OK nếu toàn bộ 9 tham số được phục hồi thành công
 */
RS485_Status_t GD200A_DisableOptimizeForShrimpFarm(uint8_t slave_addr);

/**
 * @brief  Chuyển đổi kênh điều khiển sang chế độ Manual (Keypad)
 *         P00.01 = 0 (Keypad run command)
 *         P00.06 = 0 (Keypad frequency set)
 * @param  slave_addr  Địa chỉ slave của biến tần
 * @return RS485_OK nếu thành công
 */
RS485_Status_t GD200A_SetControlManual(uint8_t slave_addr);

/**
 * @brief  Chuyển đổi kênh điều khiển sang chế độ Auto (Modbus RS485)
 *         P00.01 = 2 (Communication run command)
 *         P00.06 = 8 (Communication frequency set)
 * @param  slave_addr  Địa chỉ slave của biến tần
 * @return RS485_OK nếu thành công
 */
RS485_Status_t GD200A_SetControlAuto(uint8_t slave_addr);

#ifdef __cplusplus
}
#endif

