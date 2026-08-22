#ifndef __ILI9341_H
#define __ILI9341_H

#include "stm32f4xx_hal.h"

// Kích thước màn hình
#define ILI9341_WIDTH  320
#define ILI9341_HEIGHT 240

// Định nghĩa màu RGB565 cơ bản
#define COLOR_BLACK       0x0000
#define COLOR_WHITE       0xFFFF
#define COLOR_RED         0xF800
#define COLOR_GREEN       0x07E0
#define COLOR_BLUE        0x001F

// Cấu hình các chân điều khiển (Sửa theo Pinout mới)
// CS: PB12, DC: PB14, RST: PB10
#define ILI9341_CS_PORT   GPIOB
#define ILI9341_CS_PIN    GPIO_PIN_12

#define ILI9341_DC_PORT   GPIOB
#define ILI9341_DC_PIN    GPIO_PIN_14

#define ILI9341_RST_PORT  GPIOB
#define ILI9341_RST_PIN   GPIO_PIN_10

// Macros điều khiển chân
#define CS_LOW()      HAL_GPIO_WritePin(ILI9341_CS_PORT, ILI9341_CS_PIN, GPIO_PIN_RESET)
#define CS_HIGH()     HAL_GPIO_WritePin(ILI9341_CS_PORT, ILI9341_CS_PIN, GPIO_PIN_SET)

#define DC_COMMAND()  HAL_GPIO_WritePin(ILI9341_DC_PORT, ILI9341_DC_PIN, GPIO_PIN_RESET)
#define DC_DATA()     HAL_GPIO_WritePin(ILI9341_DC_PORT, ILI9341_DC_PIN, GPIO_PIN_SET)

#define RST_LOW()     HAL_GPIO_WritePin(ILI9341_RST_PORT, ILI9341_RST_PIN, GPIO_PIN_RESET)
#define RST_HIGH()    HAL_GPIO_WritePin(ILI9341_RST_PORT, ILI9341_RST_PIN, GPIO_PIN_SET)

// Prototype các hàm
void ILI9341_Init(SPI_HandleTypeDef *hspi);
void ILI9341_SetWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
void ILI9341_DrawBitmap(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t *bitmap);
void ILI9341_FillScreen(uint16_t color);

extern volatile uint8_t spi_dma_cplt;
void ILI9341_WriteBuffer_DMA(uint8_t *data, uint32_t len);

#endif // ILI9341_H
