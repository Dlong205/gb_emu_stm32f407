#include "ili9341.h"

static SPI_HandleTypeDef *ili9341_hspi;

static void ILI9341_WriteCommand(uint8_t cmd) {
    DC_COMMAND();
    CS_LOW();
    HAL_SPI_Transmit(ili9341_hspi, &cmd, 1, HAL_MAX_DELAY);
    CS_HIGH();
}

static void ILI9341_WriteData(uint8_t data) {
    DC_DATA();
    CS_LOW();
    HAL_SPI_Transmit(ili9341_hspi, &data, 1, HAL_MAX_DELAY);
    CS_HIGH();
}

static void ILI9341_WriteBuffer(uint8_t *data, uint32_t len) {
    DC_DATA();
    CS_LOW();
    HAL_SPI_Transmit(ili9341_hspi, data, len, HAL_MAX_DELAY);
    CS_HIGH();
}

volatile uint8_t spi_dma_cplt = 1;

void ILI9341_WriteBuffer_DMA(uint8_t *data, uint32_t len) {
    spi_dma_cplt = 0;
    DC_DATA();
    CS_LOW();
    HAL_SPI_Transmit_DMA(ili9341_hspi, data, len);
}

// Hàm callback khi DMA truyền xong
void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi) {
    if (hspi == ili9341_hspi) {
        CS_HIGH();
        spi_dma_cplt = 1;
    }
}

void ILI9341_Init(SPI_HandleTypeDef *hspi) {
    ili9341_hspi = hspi;

    // Reset
    RST_HIGH();
    HAL_Delay(5);
    RST_LOW();
    HAL_Delay(20);
    RST_HIGH();
    HAL_Delay(150);

    // Initialization sequence
    ILI9341_WriteCommand(0xEF);
    ILI9341_WriteData(0x03);
    ILI9341_WriteData(0x80);
    ILI9341_WriteData(0x02);

    ILI9341_WriteCommand(0xCF);
    ILI9341_WriteData(0x00);
    ILI9341_WriteData(0XC1);
    ILI9341_WriteData(0X30);

    ILI9341_WriteCommand(0xED);
    ILI9341_WriteData(0x64);
    ILI9341_WriteData(0x03);
    ILI9341_WriteData(0X12);
    ILI9341_WriteData(0X81);

    ILI9341_WriteCommand(0xE8);
    ILI9341_WriteData(0x85);
    ILI9341_WriteData(0x00);
    ILI9341_WriteData(0x78);

    ILI9341_WriteCommand(0xCB);
    ILI9341_WriteData(0x39);
    ILI9341_WriteData(0x2C);
    ILI9341_WriteData(0x00);
    ILI9341_WriteData(0x34);
    ILI9341_WriteData(0x02);

    ILI9341_WriteCommand(0xF7);
    ILI9341_WriteData(0x20);

    ILI9341_WriteCommand(0xEA);
    ILI9341_WriteData(0x00);
    ILI9341_WriteData(0x00);

    ILI9341_WriteCommand(0xC0);    //Power control
    ILI9341_WriteData(0x23);   //VRH[5:0]

    ILI9341_WriteCommand(0xC1);    //Power control
    ILI9341_WriteData(0x10);   //SAP[2:0];BT[3:0]

    ILI9341_WriteCommand(0xC5);    //VCM control
    ILI9341_WriteData(0x3e);
    ILI9341_WriteData(0x28);

    ILI9341_WriteCommand(0xC7);    //VCM control2
    ILI9341_WriteData(0x86);

    ILI9341_WriteCommand(0x36);    // Memory Access Control
    // Quay dọc: 0x48. Quay ngang (Landscape): 0x28 (hoặc 0xE8)
    ILI9341_WriteData(0xE8); // Landscape: MV=1, MX=1, MY=1, BGR=1 (hoặc tuỳ màn hình)

    ILI9341_WriteCommand(0x3A);
    ILI9341_WriteData(0x55); // 16-bit color

    ILI9341_WriteCommand(0xB1);
    ILI9341_WriteData(0x00);
    ILI9341_WriteData(0x18);

    ILI9341_WriteCommand(0xB6);    // Display Function Control
    ILI9341_WriteData(0x08);
    ILI9341_WriteData(0x82);
    ILI9341_WriteData(0x27);

    ILI9341_WriteCommand(0x11);    //Exit Sleep
    HAL_Delay(120);

    ILI9341_WriteCommand(0x29);    //Display on
}

void ILI9341_SetWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    ILI9341_WriteCommand(0x2A); // Column addr set
    ILI9341_WriteData(x0 >> 8);
    ILI9341_WriteData(x0 & 0xFF);
    ILI9341_WriteData(x1 >> 8);
    ILI9341_WriteData(x1 & 0xFF);

    ILI9341_WriteCommand(0x2B); // Row addr set
    ILI9341_WriteData(y0 >> 8);
    ILI9341_WriteData(y0 & 0xFF);
    ILI9341_WriteData(y1 >> 8);
    ILI9341_WriteData(y1 & 0xFF);

    ILI9341_WriteCommand(0x2C); // write to RAM
}

void ILI9341_DrawBitmap(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
    ILI9341_SetWindow(x, y, x + w - 1, y + h - 1);
    
    // Swap bytes for RGB565 endianness if necessary. STM32 is Little Endian.
    // Assuming the bitmap is already constructed in the correct byte order.
    uint32_t len = w * h * 2;
    ILI9341_WriteBuffer((uint8_t*)bitmap, len);
}

void ILI9341_FillScreen(uint16_t color) {
    ILI9341_SetWindow(0, 0, ILI9341_WIDTH - 1, ILI9341_HEIGHT - 1);
    
    // Đảo byte cho RGB565
    uint8_t hi = color >> 8, lo = color & 0xFF;
    
    // Vì tốc độ, chia ra đổ từng khối nhỏ
    uint32_t pixels = ILI9341_WIDTH * ILI9341_HEIGHT;
    uint8_t buffer[1024]; // 512 pixels
    for(int i = 0; i < 512; i++) {
        buffer[i*2] = hi;
        buffer[i*2+1] = lo;
    }
    
    while(pixels > 0) {
        uint16_t chunk = (pixels > 512) ? 512 : pixels;
        ILI9341_WriteBuffer(buffer, chunk * 2);
        pixels -= chunk;
    }
}
