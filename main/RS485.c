/**
 * @file    RS485.c
 * @brief   Thư viện điều khiển biến tần INVT GD200A qua giao thức Modbus RTU / RS485
 * @version 1.0.0
 * @date    2025
 *
 * @details
 *  Triển khai đầy đủ Modbus RTU Master trên ESP-IDF v5.x.
 *  Sử dụng UART half-duplex với GPIO DE/RE để điều khiển hướng RS485.
 *
 *  Tham chiếu: GD200A Manual V2.8 – Chapter 9 (Communication Protocol)
 */

#include "RS485.h"

/* ============================================================
 * Private TAG cho ESP_LOG
 * ============================================================ */
static const char *TAG = "GD200A_RS485";

/* ============================================================
 * Biến trạng thái nội bộ
 * ============================================================ */
static bool         s_rs485_initialized = false;
static SemaphoreHandle_t s_rs485_mutex  = NULL;

/* ============================================================
 * Private: Tính CRC16 theo chuẩn Modbus RTU
 * Thuật toán từ GD200A Manual V2.8, Section 9.3.2.2
 * ============================================================ */
static uint16_t _crc16_modbus(const uint8_t *data, uint16_t length)
{
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < length; i++)
    {
        crc ^= (uint16_t)data[i];
        for (uint8_t j = 0; j < 8; j++)
        {
            if (crc & 0x0001) crc = (crc >> 1) ^ 0xA001;    
            else crc >>= 1; 
        }
    }
    return crc;
}

/* ============================================================
 * NOTE: Không cần hàm toggle DE/RE thủ công!
 * Khi dùng UART_MODE_RS485_HALF_DUPLEX, ESP-IDF UART hardware
 * sẽ TỰ ĐỘNG kéo chân RTS (= RS485_GPIO_DIR) HIGH khi TX
 * và thả về LOW khi RX xong. Chip TEINVD1400DR:
 *   DE (active HIGH) + RE (active LOW) nối chung → một tín hiệu DIR
 *   DIR HIGH = TX mode, DIR LOW = RX mode → khớp với RTS của ESP-IDF.
 * ============================================================ */

/* ============================================================
 * Private: Gửi frame và chờ nhận phản hồi (giao tiếp cơ bản)
 * ============================================================ */
static RS485_Status_t _rs485_transact(const uint8_t *tx_buf, uint8_t tx_len, uint8_t *rx_buf, uint8_t *rx_len_out, uint8_t expected_len)
{
    if (!s_rs485_initialized) return RS485_ERR_NO_INIT;

    /* Xóa buffer RX trước khi gửi */
    uart_flush_input(RS485_UART_PORT);

    /* Gửi frame:
     * DE/RE (GPIO_DIR / RTS) được ESP-IDF UART hardware tự động điều khiển:
     *  - Kéo HIGH khi UART bắt đầu truyền byte
     *  - Thả LOW sau khi truyền xong toàn bộ frame
     * Không cần toggle thủ công. */
    uart_write_bytes(RS485_UART_PORT, (const char *)tx_buf, tx_len);
    uart_wait_tx_done(RS485_UART_PORT, pdMS_TO_TICKS(50));

    /* Chờ nhận dữ liệu */
    int rx_len = uart_read_bytes(RS485_UART_PORT, rx_buf, RS485_UART_BUF_SIZE, pdMS_TO_TICKS(RS485_RESPONSE_TIMEOUT_MS));

    if (rx_len <= 0)
    {
        ESP_LOGW(TAG, "Timeout: no response from VFD (slave=0x%02X)", tx_buf[0]);
        return RS485_ERR_TIMEOUT;
    }

    if (rx_len_out) *rx_len_out = (uint8_t)rx_len;

    /* Kiểm tra CRC của gói nhận */
    if (rx_len >= 4)
    {
        uint16_t crc_received = (uint16_t)(rx_buf[rx_len - 1] << 8) | rx_buf[rx_len - 2];
        uint16_t crc_calc     = _crc16_modbus(rx_buf, (uint16_t)(rx_len - 2));
        if (crc_received != crc_calc)
        {
            ESP_LOGE(TAG, "CRC error: received=0x%04X, calculated=0x%04X", crc_received, crc_calc);
            return RS485_ERR_CRC;
        }
    }

    /* Kiểm tra Exception Response (bit7 của function code bị set = 1) */
    if (rx_len >= 3 && (rx_buf[1] & 0x80))
    {
        ESP_LOGE(TAG, "VFD exception response: FC=0x%02X, ExCode=0x%02X", rx_buf[1], rx_buf[2]);
        return RS485_ERR_EXCEPTION;
    }

    return RS485_OK;
}

/* ============================================================
 * Public: Khởi tạo driver RS485
 * ============================================================ */
RS485_Status_t RS485_Init(void)
{
    if (s_rs485_initialized)
    {
        ESP_LOGW(TAG, "RS485 already initialized");
        return RS485_OK;
    }

    /* Tạo mutex bảo vệ truy cập UART từ nhiều task */
    s_rs485_mutex = xSemaphoreCreateMutex();
    if (!s_rs485_mutex)
    {
        ESP_LOGE(TAG, "Failed to create mutex");
        return RS485_ERR_NO_INIT;
    }

    /* Cấu hình UART:
     * KHÔNG cấu hình GPIO_DIR thủ công!
     * Chân RTS (= RS485_GPIO_DIR) sẽ được UART driver quản lý hoàn toàn
     * sau khi gọi uart_set_mode(UART_MODE_RS485_HALF_DUPLEX). */
    uart_config_t uart_cfg =
    {
        .baud_rate  = RS485_UART_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_EVEN,       /* E,8,1 – P14.02=1 */
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(RS485_UART_PORT, RS485_UART_BUF_SIZE * 2, RS485_UART_BUF_SIZE * 2, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(RS485_UART_PORT, &uart_cfg));

    /* Truyền RS485_GPIO_DIR vào tham số RTS:
     * ESP-IDF sẽ tự động HIGH khi TX, LOW khi RX
     * Khớp với TEINVD1400DR: DE(HIGH)=TX, RE(LOW)=RX nối chung 1 chân */
    ESP_ERROR_CHECK(uart_set_pin(RS485_UART_PORT,
                                 RS485_GPIO_TX,   /* TX  = GPIO 38 */
                                 RS485_GPIO_RX,   /* RX  = GPIO 40 */
                                 RS485_GPIO_DIR,  /* RTS = GPIO 39 → DE/RE control */
                                 UART_PIN_NO_CHANGE));

    /* Kích hoạt chế độ RS485 half-duplex – driver tự điều khiển RTS */
    ESP_ERROR_CHECK(uart_set_mode(RS485_UART_PORT, UART_MODE_RS485_HALF_DUPLEX));

    s_rs485_initialized = true;
    ESP_LOGI(TAG, "RS485 initialized: UART%d, Baud=%d, TX=%d, RX=%d, DIR=%d", RS485_UART_PORT, RS485_UART_BAUD_RATE, RS485_GPIO_TX, RS485_GPIO_RX, RS485_GPIO_DIR);
    return RS485_OK;
}

/* ============================================================
 * Public: Giải phóng driver
 * ============================================================ */
void RS485_Deinit(void)
{
    if (!s_rs485_initialized) return;
    uart_driver_delete(RS485_UART_PORT);
    if (s_rs485_mutex)
    {
        vSemaphoreDelete(s_rs485_mutex);
        s_rs485_mutex = NULL;
    }
    s_rs485_initialized = false;
    ESP_LOGI(TAG, "RS485 deinitialized");
}

/* ============================================================
 * Public: Modbus FC03 – Đọc N thanh ghi
 * ============================================================ */
RS485_Status_t Modbus_ReadRegisters(uint8_t slave_addr, uint16_t reg_addr, uint8_t reg_count, uint16_t *out_data)
{
    if (!out_data || reg_count == 0 || reg_count > 16) return RS485_ERR_PARAM;

    /* Đóng gói frame request FC03 */
    uint8_t tx_buf[8];
    tx_buf[0] = slave_addr;
    tx_buf[1] = MODBUS_FC_READ_HOLDING;
    tx_buf[2] = (reg_addr >> 8) & 0xFF;
    tx_buf[3] =  reg_addr       & 0xFF;
    tx_buf[4] = 0x00;
    tx_buf[5] = reg_count;
    uint16_t crc = _crc16_modbus(tx_buf, 6);
    tx_buf[6] = (uint8_t)(crc & 0xFF);         /* CRC Low byte trước */
    tx_buf[7] = (uint8_t)((crc >> 8) & 0xFF);  /* CRC High byte sau */

    /* Số byte phản hồi kỳ vọng: ADDR(1) + FC(1) + ByteCount(1) + Data(N*2) + CRC(2) */
    uint8_t expected_len = 5 + reg_count * 2;

    uint8_t rx_buf[64] = {0};
    uint8_t rx_len     = 0;

    RS485_Status_t ret = RS485_ERR_TIMEOUT;

    if (xSemaphoreTake(s_rs485_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return RS485_ERR_TIMEOUT;
    for (int retry = 0; retry < RS485_RETRY_COUNT; retry++)
    {
        ret = _rs485_transact(tx_buf, sizeof(tx_buf), rx_buf, &rx_len, expected_len);
        if (ret == RS485_OK) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (ret == RS485_OK)
    {
        /* Giải mã dữ liệu: mỗi thanh ghi 2 bytes (High byte trước) */
        for (uint8_t i = 0; i < reg_count; i++)
        {
            out_data[i] = ((uint16_t)rx_buf[3 + i * 2] << 8) | rx_buf[4 + i * 2];
        }
    }

    xSemaphoreGive(s_rs485_mutex);
    return ret;
}

/* ============================================================
 * Public: Modbus FC06 – Ghi 1 thanh ghi
 * ============================================================ */
RS485_Status_t Modbus_WriteSingleReg(uint8_t slave_addr, uint16_t reg_addr, uint16_t value)
{
    /* Đóng gói frame request FC06 */
    uint8_t tx_buf[8];
    tx_buf[0] = slave_addr;
    tx_buf[1] = MODBUS_FC_WRITE_SINGLE;
    tx_buf[2] = (reg_addr >> 8) & 0xFF;
    tx_buf[3] =  reg_addr       & 0xFF;
    tx_buf[4] = (value >> 8)    & 0xFF;
    tx_buf[5] =  value          & 0xFF;
    uint16_t crc = _crc16_modbus(tx_buf, 6);
    tx_buf[6] = (uint8_t)(crc & 0xFF);
    tx_buf[7] = (uint8_t)((crc >> 8) & 0xFF);

    uint8_t rx_buf[16] = {0};
    uint8_t rx_len     = 0;

    RS485_Status_t ret = RS485_ERR_TIMEOUT;

    if (xSemaphoreTake(s_rs485_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return RS485_ERR_TIMEOUT;
    for (int retry = 0; retry < RS485_RETRY_COUNT; retry++)
    {
        /* FC06 phản hồi echo lại y hệt frame gửi (8 bytes) */
        ret = _rs485_transact(tx_buf, sizeof(tx_buf), rx_buf, &rx_len, 8);
        if (ret == RS485_OK) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    xSemaphoreGive(s_rs485_mutex);
    return ret;
}

/* ============================================================
 * Public: Modbus FC10 – Ghi nhiều thanh ghi liên tiếp
 * ============================================================ */
RS485_Status_t Modbus_WriteMultipleRegs(uint8_t slave_addr, uint16_t reg_addr, uint8_t reg_count, uint16_t *data)
{
    if (!data || reg_count == 0 || reg_count > 16) return RS485_ERR_PARAM;

    /* Kích thước frame: ADDR(1) + FC(1) + RegAddr(2) + RegCnt(2) + ByteCnt(1) + Data(N*2) + CRC(2) */
    uint8_t byte_count = reg_count * 2;
    uint8_t tx_len     = 9 + byte_count;
    uint8_t tx_buf[64] = {0};

    tx_buf[0] = slave_addr;
    tx_buf[1] = MODBUS_FC_WRITE_MULTIPLE;
    tx_buf[2] = (reg_addr >> 8) & 0xFF;
    tx_buf[3] =  reg_addr       & 0xFF;
    tx_buf[4] = 0x00;
    tx_buf[5] = reg_count;
    tx_buf[6] = byte_count;

    for (uint8_t i = 0; i < reg_count; i++)
    {
        tx_buf[7 + i * 2] = (data[i] >> 8) & 0xFF;
        tx_buf[8 + i * 2] =  data[i]       & 0xFF;
    }

    uint16_t crc = _crc16_modbus(tx_buf, tx_len - 2);
    tx_buf[tx_len - 2] = (uint8_t)(crc & 0xFF);
    tx_buf[tx_len - 1] = (uint8_t)((crc >> 8) & 0xFF);

    uint8_t rx_buf[16] = {0};
    uint8_t rx_len     = 0;

    RS485_Status_t ret = RS485_ERR_TIMEOUT;

    if (xSemaphoreTake(s_rs485_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return RS485_ERR_TIMEOUT;
    for (int retry = 0; retry < RS485_RETRY_COUNT; retry++)
    {
        /* FC10 phản hồi: ADDR(1)+FC(1)+StartAddr(2)+RegCnt(2)+CRC(2) = 8 bytes */
        ret = _rs485_transact(tx_buf, tx_len, rx_buf, &rx_len, 8);
        if (ret == RS485_OK) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    xSemaphoreGive(s_rs485_mutex);
    return ret;
}

/* ============================================================
 * Public: Gửi lệnh điều khiển vận hành (ghi vào 0x2000)
 * ============================================================ */
static RS485_Status_t _send_control_cmd(uint8_t slave_addr, uint16_t cmd)
{
    ESP_LOGI(TAG, "Control cmd: slave=0x%02X, cmd=0x%04X", slave_addr, cmd);
    return Modbus_WriteSingleReg(slave_addr, GD200A_REG_CTRL_CMD, cmd);
}

RS485_Status_t GD200A_RunForward(uint8_t slave_addr)
{
    return _send_control_cmd(slave_addr, GD200A_CMD_FORWARD_RUN);
}

RS485_Status_t GD200A_RunReverse(uint8_t slave_addr)
{
    return _send_control_cmd(slave_addr, GD200A_CMD_REVERSE_RUN);
}

RS485_Status_t GD200A_Stop(uint8_t slave_addr)
{
    return _send_control_cmd(slave_addr, GD200A_CMD_STOP);
}

RS485_Status_t GD200A_CoastStop(uint8_t slave_addr)
{
    return _send_control_cmd(slave_addr, GD200A_CMD_COAST_STOP);
}

RS485_Status_t GD200A_FaultReset(uint8_t slave_addr)
{
    return _send_control_cmd(slave_addr, GD200A_CMD_FAULT_RESET);
}

/* ============================================================
 * Public: Cài đặt tần số
 * Ref: GD200A Manual 9.5.2 – Register 0x2001
 * Đơn vị: 0.01Hz → 50Hz = 5000
 * ============================================================ */
RS485_Status_t GD200A_SetFrequency(uint8_t slave_addr, float freq_hz)
{
    if (freq_hz < 0.0f || freq_hz > 400.0f)
    {
        ESP_LOGE(TAG, "Invalid frequency: %.2f Hz", freq_hz);
        return RS485_ERR_PARAM;
    }
    uint16_t raw_val = (uint16_t)(freq_hz * 100.0f);   /* unit: 0.01Hz */
    ESP_LOGI(TAG, "Set frequency: %.2f Hz (raw=%d)", freq_hz, raw_val);
    return Modbus_WriteSingleReg(slave_addr, GD200A_REG_FREQ_SET, raw_val);
}

/* ============================================================
 * Public: Cài đặt thời gian tăng tốc (Soft Start)
 * Ref: GD200A Manual – P00.11, Register 0x000B
 * Ghi lên địa chỉ 0x800B để chỉ cập nhật RAM (không lưu EEPROM)
 * Đơn vị: 0.1s → 5.0s = 50
 * ============================================================ */
RS485_Status_t GD200A_SetAccelTime(uint8_t slave_addr, float time_s)
{
    if (time_s < 0.0f || time_s > 3600.0f)
    {
        ESP_LOGE(TAG, "Invalid accel time: %.1f s", time_s);
        return RS485_ERR_PARAM;
    }
    /* Ghi vào 0x800B = RAM only (không lưu EEPROM, bảo vệ bộ nhớ flash) */
    uint16_t raw_val = (uint16_t)(time_s * 10.0f);  /* unit: 0.1s */
    ESP_LOGI(TAG, "Set accel time: %.1f s (raw=%d)", time_s, raw_val);
    return Modbus_WriteSingleReg(slave_addr, 0x800B, raw_val);
}

/* ============================================================
 * Public: Cài đặt thời gian giảm tốc (Soft Stop)
 * Ref: GD200A Manual – P00.12, Register 0x000C
 * Ghi lên 0x800C để cập nhật RAM only
 * ============================================================ */
RS485_Status_t GD200A_SetDecelTime(uint8_t slave_addr, float time_s)
{
    if (time_s < 0.0f || time_s > 3600.0f)
    {
        ESP_LOGE(TAG, "Invalid decel time: %.1f s", time_s);
        return RS485_ERR_PARAM;
    }
    uint16_t raw_val = (uint16_t)(time_s * 10.0f);  /* unit: 0.1s */
    ESP_LOGI(TAG, "Set decel time: %.1f s (raw=%d)", time_s, raw_val);
    return Modbus_WriteSingleReg(slave_addr, 0x800C, raw_val);
}

/* ============================================================
 * Public: Cài đặt đồng thời Accel + Decel time
 * Dùng FC10 (Write Multiple) để tiết kiệm giao tiếp
 * ============================================================ */
RS485_Status_t GD200A_SetSoftStartStop(uint8_t slave_addr, float accel_time_s, float decel_time_s)
{
    if (accel_time_s < 0.0f || accel_time_s > 3600.0f || decel_time_s < 0.0f || decel_time_s > 3600.0f) return RS485_ERR_PARAM;

    uint16_t data[2];
    data[0] = (uint16_t)(accel_time_s * 10.0f);  /* P00.11: unit 0.1s */
    data[1] = (uint16_t)(decel_time_s * 10.0f);  /* P00.12: unit 0.1s */

    ESP_LOGI(TAG, "Set soft start/stop: accel=%.1fs, decel=%.1fs", accel_time_s, decel_time_s);

    /* Ghi vào RAM only: địa chỉ 0x800B cho P00.11, 0x800C cho P00.12
     * Chú ý: FC10 ghi các địa chỉ liên tiếp, cần ghi 0x000B và 0x000C trực tiếp
     * (không thể dùng FC10 cho dải địa chỉ 0x800B vì chúng không liên tiếp theo nghĩa thường)
     * Để an toàn: ghi lần lượt bằng FC06 */
    RS485_Status_t ret;
    ret = Modbus_WriteSingleReg(slave_addr, 0x800B, data[0]);
    if (ret != RS485_OK) return ret;

    ret = Modbus_WriteSingleReg(slave_addr, 0x800C, data[1]);
    return ret;
}

/* ============================================================
 * Public: Cấu hình tự khởi động lại sau khi có điện (P01.21 & P01.22)
 * ============================================================ */
RS485_Status_t GD200A_SetAutoRunAfterPowerBack(uint8_t slave_addr, uint8_t enable, float delay_s)
{
    if (enable > 1) return RS485_ERR_PARAM;
    if (delay_s < 0.0f || delay_s > 3600.0f) return RS485_ERR_PARAM;

    RS485_Status_t ret;
    ret = Modbus_WriteSingleReg(slave_addr, 0x0115, enable);
    if (ret != RS485_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(50)); // Delay nhỏ để EEPROM ghi kịp

    uint16_t delay_raw = (uint16_t)(delay_s * 10.0f);
    ret = Modbus_WriteSingleReg(slave_addr, 0x0116, delay_raw);
    if (ret != RS485_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_LOGI(TAG, "Configure Auto Run after Power Back: enable=%d, delay=%.1fs", enable, delay_s);
    return RS485_OK;
}

/* ============================================================
 * Public: Đọc toàn bộ thông số vận hành (khối 7 registers 0x3000–0x3006)
 * ============================================================ */
RS485_Status_t GD200A_ReadStatus(uint8_t slave_addr, GD200A_Status_t *status)
{
    if (!status) return RS485_ERR_PARAM;

    uint16_t regs[7] = {0};

    /* Đọc khối 7 thanh ghi monitor: 0x3000 – 0x3006 */
    RS485_Status_t ret = Modbus_ReadRegisters(slave_addr, GD200A_REG_FREQ_ACT, 7, regs);
    if (ret != RS485_OK) return ret;

    status->freq_actual_hz    = (double)regs[0] / 100.0;   /* 0x3000: unit 0.01Hz */
    status->freq_set_hz       = (double)regs[1] / 100.0;   /* 0x3001: unit 0.01Hz */
    status->bus_voltage_v     = (double)regs[2] / 10.0;     /* 0x3002: unit 0.1V */
    status->output_voltage_v  = regs[3];                    /* 0x3003: unit V */
    status->output_current_a  = (double)regs[4] / 10.0;    /* 0x3004: unit 0.1A */
    status->motor_speed_rpm   = regs[5];                    /* 0x3005: unit RPM */
    status->output_power_pct  = (double)((int16_t)regs[6]) / 10.0;    /* 0x3006: unit 0.1% (signed) */

    /* Đọc thêm Status Word 1 (0x2100) và Fault Code (0x2102) */
    uint16_t status_regs[3] = {0};
    ret = Modbus_ReadRegisters(slave_addr, GD200A_REG_STATUS_1, 3, status_regs);
    if (ret == RS485_OK)
    {
        status->status_word = status_regs[0];               /* 0x2100 */
        status->fault_code  = status_regs[2];               /* 0x2102 */
        status->run_command_channel = (status_regs[1] >> 5) & 0x03; /* 0x2101 Bit 5-6: 0=keypad, 1=terminal, 2=comm */

        status->is_running = (status_regs[0] == GD200A_STATUS_FORWARD || status_regs[0] == GD200A_STATUS_REVERSE);
        status->is_fwd     = (status_regs[0] == GD200A_STATUS_FORWARD);
        status->is_fault   = (status_regs[0] == GD200A_STATUS_FAULT);
    }

    /* Đọc thêm Accel Time 1 (P00.11 - 0x000B) và Decel Time 1 (P00.12 - 0x000C) */
    uint16_t time_regs[2] = {0};
    RS485_Status_t time_ret = Modbus_ReadRegisters(slave_addr, 0x000B, 2, time_regs);
    if (time_ret == RS485_OK)
    {
        status->accel_time_s = (double)time_regs[0] / 10.0; /* unit: 0.1s */
        status->decel_time_s = (double)time_regs[1] / 10.0; /* unit: 0.1s */
    }
    else
    {
        status->accel_time_s = 0.0;
        status->decel_time_s = 0.0;
    }

    /* Đọc thêm P01.21 (0x0115) và P01.22 (0x0116) */
    uint16_t autorun_regs[2] = {0};
    RS485_Status_t autorun_ret = Modbus_ReadRegisters(slave_addr, 0x0115, 2, autorun_regs);
    if (autorun_ret == RS485_OK)
    {
        status->auto_run_enable = autorun_regs[0];
        status->auto_run_delay_s = (double)autorun_regs[1] / 10.0;
    }
    else
    {
        status->auto_run_enable = 0;
        status->auto_run_delay_s = 0.0;
    }

    /* Đọc thêm điện năng tiêu thụ tích luỹ P07.15 (0x070F) và P07.16 (0x0710) */
    uint16_t energy_regs[2] = {0};
    RS485_Status_t energy_ret = Modbus_ReadRegisters(slave_addr, 0x070F, 2, energy_regs);
    if (energy_ret == RS485_OK)
    {
        status->cumulative_energy_kwh = (double)energy_regs[0] * 1000.0 + (double)energy_regs[1] / 10.0;
    }
    else
    {
        status->cumulative_energy_kwh = -1.0;
    }

    return RS485_OK;
}

/* ============================================================
 * Public: Đọc tần số thực tế
 * ============================================================ */
RS485_Status_t GD200A_ReadFrequency(uint8_t slave_addr, float *freq_hz)
{
    if (!freq_hz) return RS485_ERR_PARAM;
    uint16_t raw = 0;
    RS485_Status_t ret = Modbus_ReadRegisters(slave_addr, GD200A_REG_FREQ_ACT, 1, &raw);
    if (ret == RS485_OK) *freq_hz = (float)raw / 100.0f;
    return ret;
}

/* ============================================================
 * Public: Đọc dòng điện ngõ ra
 * ============================================================ */
RS485_Status_t GD200A_ReadCurrent(uint8_t slave_addr, float *current_a)
{
    if (!current_a) return RS485_ERR_PARAM;
    uint16_t raw = 0;
    RS485_Status_t ret = Modbus_ReadRegisters(slave_addr, GD200A_REG_OUTPUT_CURRENT, 1, &raw);
    if (ret == RS485_OK) *current_a = (float)raw / 10.0f;    /* unit: 0.1A */
    return ret;
}

/* ============================================================
 * Public: Đọc điện áp ngõ ra
 * ============================================================ */
RS485_Status_t GD200A_ReadOutputVoltage(uint8_t slave_addr, uint16_t *voltage_v)
{
    if (!voltage_v) return RS485_ERR_PARAM;
    uint16_t raw = 0;
    RS485_Status_t ret = Modbus_ReadRegisters(slave_addr, GD200A_REG_OUTPUT_VOLTAGE, 1, &raw);
    if (ret == RS485_OK) *voltage_v = raw;
    return ret;
}

/* ============================================================
 * Public: Đọc mã lỗi
 * ============================================================ */
RS485_Status_t GD200A_ReadFaultCode(uint8_t slave_addr, uint16_t *fault_code)
{
    if (!fault_code) return RS485_ERR_PARAM;
    uint16_t raw = 0;
    RS485_Status_t ret = Modbus_ReadRegisters(slave_addr, GD200A_REG_FAULT_CODE_COMM, 1, &raw);
    if (ret == RS485_OK) *fault_code = raw;
    return ret;
}

/* ============================================================
 * Public: Tra cứu mô tả mã lỗi (Ref: GD200A Manual – Fault List)
 * ============================================================ */
const char* GD200A_GetFaultString(uint16_t fault_code)
{
    switch ((GD200A_FaultCode_t)fault_code)
    {
        case GD200A_FAULT_NONE:         return "No Fault";
        case GD200A_FAULT_OVERCUR_ACC:  return "Err01: Overcurrent during acceleration";
        case GD200A_FAULT_OVERCUR_DEC:  return "Err02: Overcurrent during deceleration";
        case GD200A_FAULT_OVERCUR_CST:  return "Err03: Overcurrent at constant speed";
        case GD200A_FAULT_OVERVOLT_ACC: return "Err04: Overvoltage during acceleration";
        case GD200A_FAULT_OVERVOLT_DEC: return "Err05: Overvoltage during deceleration";
        case GD200A_FAULT_OVERVOLT_CST: return "Err06: Overvoltage at constant speed";
        case GD200A_FAULT_UNDERVOLT:    return "Err07: DC Bus undervoltage";
        case GD200A_FAULT_OVERLOAD:     return "Err09: VFD overload";
        case GD200A_FAULT_MOTOR_OVL:    return "Err10: Motor overload";
        case GD200A_FAULT_PHASE_LOSS:   return "Err21: Input phase loss";
        case GD200A_FAULT_OVERHEAT:     return "Err24: VFD overheat";
        case GD200A_FAULT_COMM_ERR:     return "Err40: Modbus communication error";
        default:                        return "Unknown fault code";
    }
}

RS485_Status_t GD200A_EnableOptimizeForShrimpFarm(uint8_t slave_addr)
{
    RS485_Status_t ret;
    ESP_LOGI(TAG, "Starting Shrimp Farm Optimization for VFD (slave=0x%02X)...", slave_addr);

    // 1. Tự khởi động lại sau khi có điện: P01.21 = 1, P01.22 = 3.0s (30 raw)
    ret = Modbus_WriteSingleReg(slave_addr, 0x0115, 1);
    if (ret != RS485_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(50)); // Delay nhỏ để EEPROM ghi kịp
    
    ret = Modbus_WriteSingleReg(slave_addr, 0x0116, 30);
    if (ret != RS485_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(50));

    // 2. Tự động reset lỗi thoáng qua: P08.28 = 5 (lần), P08.29 = 2.0s (20 raw)
    ret = Modbus_WriteSingleReg(slave_addr, 0x081C, 5);
    if (ret != RS485_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(50));

    ret = Modbus_WriteSingleReg(slave_addr, 0x081D, 20);
    if (ret != RS485_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(50));

    // 3. Tự động ổn áp (AVR): P00.16 = 1
    ret = Modbus_WriteSingleReg(slave_addr, 0x0010, 1);
    if (ret != RS485_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(50));

    // 4. Chế độ tiết kiệm điện cho quạt/bơm: P04.00 = 4 (bậc 2.0), P04.26 = 1
    ret = Modbus_WriteSingleReg(slave_addr, 0x0400, 4);
    if (ret != RS485_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(50));

    ret = Modbus_WriteSingleReg(slave_addr, 0x041A, 1);
    if (ret != RS485_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(50));

    // 5. Bảo vệ quá tải động cơ thường: P02.26 = 1, P02.27 = 100.0% (1000 raw)
    ret = Modbus_WriteSingleReg(slave_addr, 0x021A, 1);
    if (ret != RS485_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(50));

    ret = Modbus_WriteSingleReg(slave_addr, 0x021B, 1000);
    if (ret != RS485_OK) return ret;

    ESP_LOGI(TAG, "VFD Shrimp Farm Optimization Completed Successfully!");
    return RS485_OK;
}

RS485_Status_t GD200A_DisableOptimizeForShrimpFarm(uint8_t slave_addr)
{
    RS485_Status_t ret;
    ESP_LOGI(TAG, "Restoring VFD parameters to Factory Defaults for Shrimp Farm parameters (slave=0x%02X)...", slave_addr);

    // 1. Tắt tự khởi động lại: P01.21 = 0, P01.22 = 1.0s (10 raw)
    ret = Modbus_WriteSingleReg(slave_addr, 0x0115, 0);
    if (ret != RS485_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(50));
    
    ret = Modbus_WriteSingleReg(slave_addr, 0x0116, 10);
    if (ret != RS485_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(50));

    // 2. Tắt tự động reset lỗi: P08.28 = 0, P08.29 = 1.0s (10 raw)
    ret = Modbus_WriteSingleReg(slave_addr, 0x081C, 0);
    if (ret != RS485_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(50));

    ret = Modbus_WriteSingleReg(slave_addr, 0x081D, 10);
    if (ret != RS485_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(50));

    // 3. Tự động ổn áp (AVR): P00.16 = 1 (mặc định)
    ret = Modbus_WriteSingleReg(slave_addr, 0x0010, 1);
    if (ret != RS485_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(50));

    // 4. Khôi phục V/F tuyến tính và Tắt tiết kiệm điện: P04.00 = 0, P04.26 = 0
    ret = Modbus_WriteSingleReg(slave_addr, 0x0400, 0);
    if (ret != RS485_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(50));

    ret = Modbus_WriteSingleReg(slave_addr, 0x041A, 0);
    if (ret != RS485_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(50));

    // 5. Khôi phục bảo vệ quá tải động cơ biến tần mặc định: P02.26 = 2, P02.27 = 100.0% (1000 raw)
    ret = Modbus_WriteSingleReg(slave_addr, 0x021A, 2);
    if (ret != RS485_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(50));

    ret = Modbus_WriteSingleReg(slave_addr, 0x021B, 1000);
    if (ret != RS485_OK) return ret;

    ESP_LOGI(TAG, "VFD Shrimp Farm parameters restored to defaults successfully!");
    return RS485_OK;
}

RS485_Status_t GD200A_SetControlManual(uint8_t slave_addr)
{
    ESP_LOGI(TAG, "Switching VFD to Manual Mode (slave=0x%02X)...", slave_addr);
    RS485_Status_t ret;
    
    // Set run command source to Keypad (P00.01 = 0)
    ret = Modbus_WriteSingleReg(slave_addr, 0x0001, 0);
    if (ret != RS485_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(50));

    // Set frequency command source to Keypad (P00.06 = 0)
    ret = Modbus_WriteSingleReg(slave_addr, 0x0006, 0);
    return ret;
}

RS485_Status_t GD200A_SetControlAuto(uint8_t slave_addr)
{
    ESP_LOGI(TAG, "Switching VFD to Auto Mode (slave=0x%02X)...", slave_addr);
    RS485_Status_t ret;

    // Set run command source to Modbus (P00.01 = 2)
    ret = Modbus_WriteSingleReg(slave_addr, 0x0001, 2);
    if (ret != RS485_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(50));

    // Set frequency command source to Modbus (P00.06 = 8)
    ret = Modbus_WriteSingleReg(slave_addr, 0x0006, 8);
    return ret;
}

