#ifndef SSD1306_H
#define SSD1306_H

#include "stm32f4xx_hal.h"
#include "stdint.h"
#include "stdlib.h"
#include "string.h"

#define SSD1306_WIDTH 128
#define SSD1306_HEIGHT 64

#define SSD1306_I2C_ADDR_3C 0x3C
#define SSD1306_I2C_ADDR_3D 0x3D

typedef enum {
    Black = 0x00,
    White = 0x01,
    Inverse = 0x02
} SSD1306_COLOR;

typedef struct {
    uint16_t CurrentX;
    uint16_t CurrentY;
    uint8_t Inverted;
    uint8_t Initialized;
    I2C_HandleTypeDef *I2C_Handle;
    uint8_t Address;
    uint8_t Width;
    uint8_t Height;
} SSD1306_t;

typedef struct {
    const uint8_t *data;
    uint8_t width;
    uint8_t height;
} FontDef;

extern FontDef Font_5x7;
extern FontDef Font_7x10;
extern FontDef Font_11x18;
extern FontDef Font_16x26;

HAL_StatusTypeDef ssd1306_Init(SSD1306_t *dev, I2C_HandleTypeDef *i2c, uint8_t addr, uint8_t width, uint8_t height);
void ssd1306_Fill(SSD1306_t *dev, SSD1306_COLOR color);
void ssd1306_UpdateScreen(SSD1306_t *dev);
void ssd1306_DrawPixel(SSD1306_t *dev, uint16_t x, uint16_t y, SSD1306_COLOR color);
void ssd1306_SetCursor(SSD1306_t *dev, uint16_t x, uint16_t y);
char ssd1306_WriteChar(SSD1306_t *dev, char ch, FontDef font, SSD1306_COLOR color);
char ssd1306_WriteString(SSD1306_t *dev, char *str, FontDef font, SSD1306_COLOR color);
void ssd1306_DrawLine(SSD1306_t *dev, uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, SSD1306_COLOR color);
void ssd1306_DrawRectangle(SSD1306_t *dev, uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, SSD1306_COLOR color);
void ssd1306_FillRectangle(SSD1306_t *dev, uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, SSD1306_COLOR color);
void ssd1306_DrawCircle(SSD1306_t *dev, uint16_t x0, uint16_t y0, uint16_t r, SSD1306_COLOR color);
void ssd1306_FillCircle(SSD1306_t *dev, uint16_t x0, uint16_t y0, uint16_t r, SSD1306_COLOR color);
void ssd1306_InvertDisplay(SSD1306_t *dev, uint8_t invert);
void ssd1306_SetContrast(SSD1306_t *dev, uint8_t contrast);

#endif