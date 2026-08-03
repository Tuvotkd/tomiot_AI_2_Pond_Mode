#pragma once

#include "stdint.h"
#include "stdbool.h"

#define SPI_HOST                SPI2_HOST // Sử dụng VSPI host (SPI2_HOST tren ESP32-S3)

#define PIN_NUM_MISO 13
#define PIN_NUM_MOSI 11
#define PIN_NUM_CLK  12
#define PIN_NUM_CS   9

#define PIN_NUM_WP   14

#define OPCODE_WREN  0x06  // Write Enable
#define OPCODE_WRDI  0x04  // Write Disable
#define OPCODE_READ  0x03  // Read Memory
#define OPCODE_WRITE 0x02  // Write Memory
#define OPCODE_RDSR  0x05  // Read Status Register
#define OPCODE_WRSR  0x01  // Write Status Register


void FRAM_Delete_All(void);

/**
 * @brief Xóa sạch N slot thiết bị đầu tiên trong FRAM (không ảnh hưởng cấu hình WiFi/Azure)
 * @param num_slots Số lượng slot cần xóa: 10 (Mode 10->4) hoặc 4 (Mode 4->10)
 */
void FRAM_Clear_Device_Slots(uint8_t num_slots);

void User_Fram_Task();

void Fram_Write_Enable(void);

void Fram_Write_Data(uint16_t address, uint8_t *data, uint16_t size);

void Fram_Read_Device_ID(uint8_t *id_bytes, uint8_t size);

bool Fram_Read_Data(uint16_t address, uint8_t *data, uint16_t size);

bool Fram_Get_Capacity(uint32_t *capacity_bytes);

bool Fram_Init(void);
