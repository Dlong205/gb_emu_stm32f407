#include "stm32f4xx_hal.h"
#include "ili9341.h"
#include "ff.h"
#include "stdio.h"
#include "string.h"
#include "stdbool.h"

#define PEANUT_GB_HEADER_ONLY
#include "peanut_gb.h"

/* ===================== CẤU HÌNH CHUNG ===================== */
#define GB_LCD_W   160
#define GB_LCD_H   144
#define ROM_CACHE_SIZE 512

/* ===================== GLOBAL ===================== */
SPI_HandleTypeDef hspi1;
FATFS fs;
SD_HandleTypeDef hsd;
struct gb_s gb;

FIL rom_file;
static uint8_t rom_bank0[16384];
#define CACHE_LINES 32
static uint8_t rom_cache[CACHE_LINES][ROM_CACHE_SIZE];
static uint32_t rom_cache_sector[CACHE_LINES];
static bool cache_init = false;

static uint8_t cart_ram[0x8000];

static const uint16_t gb_palette[4] = {
    0xFFFF, // Trắng
    0xBDF7, // Xám nhạt
    0x632C, // Xám đậm
    0x0000  // Đen
};

static volatile uint32_t tick_ms;
static uint32_t last_fps_time, fps_count;
static char gb_title[17];
static char disp_buf[64];
static char rom_path[32];

/* ===================== PROTOTYPES ===================== */
void SystemClock_Config(void);
void MX_SPI1_Init(void);
void MX_SDIO_SD_Init(void);
void MX_Gamepad_Init(void);
uint8_t gb_rom_read_cb(struct gb_s *gb, const uint_fast32_t addr);
uint8_t gb_cart_ram_read_cb(struct gb_s *gb, const uint_fast32_t addr);
void gb_cart_ram_write_cb(struct gb_s *gb, const uint_fast32_t addr, const uint8_t val);
void gb_error_cb(struct gb_s *gb, const enum gb_error_e gb_err, const uint16_t addr);
void gb_lcd_draw_line_cb(struct gb_s *gb, const uint8_t *pixels, const uint_fast8_t line);
FRESULT open_first_rom(void);
static uint32_t crc32_compute(void);

/* ===================== MAIN ===================== */
int main(void) {
    HAL_Init();
    SystemClock_Config();
    MX_SPI1_Init();

    /* LED trên PA6 */
    __HAL_RCC_GPIOA_CLK_ENABLE();
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin = GPIO_PIN_6;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    /* Chân điều khiển TFT: PA1, PA2, PA4 */
    GPIO_InitStruct.Pin = GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_4;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET); // CS_HIGH mặc định

    HAL_Delay(100);

    /* Khởi tạo ILI9341 */
    ILI9341_Init(&hspi1);
    ILI9341_FillScreen(COLOR_BLACK);

    MX_SDIO_SD_Init();
    MX_Gamepad_Init();
    
    HAL_StatusTypeDef sd_status = HAL_OK;
    for (int attempt = 0; attempt < 3; attempt++) {
        HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_6);
        sd_status = HAL_SD_Init(&hsd);
        if (sd_status == HAL_OK) break;
        HAL_Delay(500);
    }
    if (sd_status != HAL_OK) {
        while (1) { HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_6); HAL_Delay(200); }
    }

    FRESULT fres = FR_DISK_ERR;
    for (int attempt = 0; attempt < 3 && fres != FR_OK; attempt++) {
        fres = f_mount(&fs, "0:", 1);
        if (fres != FR_OK) HAL_Delay(500);
    }
    if (fres != FR_OK) {
        while (1) { HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_6); HAL_Delay(200); }
    }

    fres = open_first_rom();
    if (fres != FR_OK) {
        while (1) { HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_6); HAL_Delay(200); }
    }

    enum gb_init_error_e gb_init_err =
        gb_init(&gb, gb_rom_read_cb, gb_cart_ram_read_cb,
                gb_cart_ram_write_cb, gb_error_cb, NULL);
    if (gb_init_err != GB_INIT_NO_ERROR) {
        while (1) { HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_6); HAL_Delay(200); }
    }

    size_t cart_ram_size = 0;
    gb_get_save_size_s(&gb, &cart_ram_size);
    if (cart_ram_size > 0x8000) cart_ram_size = 0x8000;

    gb_init_lcd(&gb, gb_lcd_draw_line_cb);

    /* ===================== MAIN LOOP ===================== */
    last_fps_time = HAL_GetTick();
    uint8_t frameskip = 0;
    while (1) {
        uint8_t joypad = 0xFF; // All released
        if (HAL_GPIO_ReadPin(GPIOE, GPIO_PIN_7) == GPIO_PIN_RESET) joypad &= ~JOYPAD_UP;
        if (HAL_GPIO_ReadPin(GPIOE, GPIO_PIN_8) == GPIO_PIN_RESET) joypad &= ~JOYPAD_DOWN;
        if (HAL_GPIO_ReadPin(GPIOE, GPIO_PIN_9) == GPIO_PIN_RESET) joypad &= ~JOYPAD_LEFT;
        if (HAL_GPIO_ReadPin(GPIOE, GPIO_PIN_10) == GPIO_PIN_RESET) joypad &= ~JOYPAD_RIGHT;
        if (HAL_GPIO_ReadPin(GPIOE, GPIO_PIN_11) == GPIO_PIN_RESET) joypad &= ~JOYPAD_A;
        if (HAL_GPIO_ReadPin(GPIOE, GPIO_PIN_12) == GPIO_PIN_RESET) joypad &= ~JOYPAD_B;
        
        if (HAL_GPIO_ReadPin(GPIOE, GPIO_PIN_4) == GPIO_PIN_RESET) joypad &= ~JOYPAD_START; // K0
        if (HAL_GPIO_ReadPin(GPIOE, GPIO_PIN_3) == GPIO_PIN_RESET) joypad &= ~JOYPAD_SELECT; // K1
        
        gb.direct.joypad = joypad;

        gb_run_frame(&gb);
        fps_count++;

        /* FPS tính nội bộ */
        if (HAL_GetTick() - last_fps_time >= 1000) {
            last_fps_time = HAL_GetTick();
            fps_count = 0;
        }
    }
}

/* ===================== CALLBACK ROM ===================== */
uint8_t gb_rom_read_cb(struct gb_s *gb, const uint_fast32_t addr) {
    if (addr < 0x4000) {
        uint32_t sector = addr / 512;
        static uint32_t bank0_loaded = 0;
        if ((bank0_loaded & (1UL << sector)) == 0) {
            UINT br = 0;
            if (f_lseek(&rom_file, sector * 512) == FR_OK) {
                f_read(&rom_file, &rom_bank0[sector * 512], 512, &br);
            }
            bank0_loaded |= (1UL << sector);
        }
        return rom_bank0[addr];
    }

    if (!cache_init) {
        for (int j = 0; j < CACHE_LINES; j++) rom_cache_sector[j] = 0xFFFFFFFF;
        cache_init = true;
    }

    uint32_t sector = addr / ROM_CACHE_SIZE;
    uint32_t index = sector % CACHE_LINES;
    
    if (rom_cache_sector[index] == sector) {
        return rom_cache[index][addr % ROM_CACHE_SIZE];
    }
    
    UINT br = 0;
    if (f_lseek(&rom_file, sector * ROM_CACHE_SIZE) == FR_OK) {
        f_read(&rom_file, rom_cache[index], ROM_CACHE_SIZE, &br);
    }
    rom_cache_sector[index] = sector;
    
    return rom_cache[index][addr % ROM_CACHE_SIZE];
}

uint8_t gb_cart_ram_read_cb(struct gb_s *gb, const uint_fast32_t addr) {
    if (addr < sizeof(cart_ram)) return cart_ram[addr];
    return 0xFF;
}

void gb_cart_ram_write_cb(struct gb_s *gb, const uint_fast32_t addr, const uint8_t val) {
    if (addr < sizeof(cart_ram)) cart_ram[addr] = val;
}

void gb_error_cb(struct gb_s *gb, const enum gb_error_e gb_err, const uint16_t addr) {
    (void)gb; (void)gb_err; (void)addr;
}

/* ===================== RENDER GB FRAME → ILI9341 ===================== */
void gb_lcd_draw_line_cb(struct gb_s *gb, const uint8_t *pixels, const uint_fast8_t line) {
    if (line >= GB_LCD_H) return;
    uint16_t line_buffer[GB_LCD_W];
    for (uint8_t x = 0; x < GB_LCD_W; x++) {
        uint8_t color_idx = pixels[x] & 0x03;
        line_buffer[x] = gb_palette[color_idx];
    }
    // Center 160x144 on 320x240
    ILI9341_DrawBitmap(80, 48 + line, GB_LCD_W, 1, line_buffer);
}

/* ===================== HELPER FILES ===================== */
static bool rom_header_valid(void) {
    static const uint8_t logo[8] = {0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B};
    uint8_t buf[8];
    UINT br = 0;
    if (f_lseek(&rom_file, 0x104) != FR_OK) return false;
    if (f_read(&rom_file, buf, 8, &br) != FR_OK || br != 8) return false;
    if (memcmp(buf, logo, 8) != 0) return false;
    uint32_t sz = f_size(&rom_file);
    return sz >= 0x8000 && (sz % 0x8000) == 0;
}

FRESULT open_first_rom(void) {
    FRESULT fres;
    static const char *known[] = { "0:POKEMON_RED.GB", "0:POKEMON_RED.gb" };
    for (size_t i = 0; i < sizeof(known)/sizeof(known[0]); i++) {
        fres = f_open(&rom_file, known[i], FA_READ | FA_OPEN_EXISTING);
        if (fres == FR_OK) {
            if (rom_header_valid()) return FR_OK;
            f_close(&rom_file);
        }
    }
    DIR dir; FILINFO fno;
    if (f_findfirst(&dir, &fno, "0:", "*.GB") != FR_OK || fno.fname[0] == 0)
        f_findfirst(&dir, &fno, "0:", "*.gb");
    
    while (fno.fname[0] != 0) {
        sprintf(disp_buf, "0:%s", fno.fname);
        fres = f_open(&rom_file, disp_buf, FA_READ | FA_OPEN_EXISTING);
        if (fres == FR_OK) {
            if (rom_header_valid()) {
                f_closedir(&dir);
                return FR_OK;
            }
            f_close(&rom_file);
        }
        if (f_findnext(&dir, &fno) != FR_OK || fno.fname[0] == 0) break;
    }
    f_closedir(&dir);
    return FR_NO_FILE;
}

static uint32_t crc32_compute(void) {
    uint32_t crc = 0xFFFFFFFF;
    uint8_t buf[512];
    if (f_lseek(&rom_file, 0) != FR_OK) return 0;
    for (;;) {
        UINT br = 0;
        if (f_read(&rom_file, buf, sizeof(buf), &br) != FR_OK || br == 0) break;
        for (UINT i = 0; i < br; i++) {
            crc ^= buf[i];
            for (int b = 0; b < 8; b++)
                crc = (crc >> 1) ^ (0xEDB88320 & (uint32_t)-(int)(crc & 1));
        }
    }
    return ~crc;
}

/* ===================== HARDWARE INIT ===================== */
void SystemClock_Config(void) {
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    RCC_OscInitStruct.HSEState = RCC_HSE_ON;
    RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    RCC_OscInitStruct.PLL.PLLM = 8;
    RCC_OscInitStruct.PLL.PLLN = 336;
    RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
    RCC_OscInitStruct.PLL.PLLQ = 7;
    HAL_RCC_OscConfig(&RCC_OscInitStruct);

    RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                                  RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;
    HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5);
}

void MX_SPI1_Init(void) {
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_SPI1_CLK_ENABLE();

    GPIO_InitTypeDef GPIO_InitStruct = {0};
    /* SPI1 GPIO Configuration
       PA5     ------> SPI1_SCK
       PA7     ------> SPI1_MOSI 
    */
    GPIO_InitStruct.Pin = GPIO_PIN_5 | GPIO_PIN_7;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF5_SPI1;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    hspi1.Instance = SPI1;
    hspi1.Init.Mode = SPI_MODE_MASTER;
    hspi1.Init.Direction = SPI_DIRECTION_2LINES;
    hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
    hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;
    hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;
    hspi1.Init.NSS = SPI_NSS_SOFT;
    // PCLK2 (84MHz) / 8 = 10.5MHz SPI (Ổn định hơn cho dây cắm test)
    hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_8;
    hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
    hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
    hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    hspi1.Init.CRCPolynomial = 10;
    HAL_SPI_Init(&hspi1);
}

void MX_SDIO_SD_Init(void) {
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_SDIO_CLK_ENABLE();

    GPIO_InitTypeDef GPIO_InitStruct = {0};

    GPIO_InitStruct.Pin = GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF12_SDIO;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_2;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF12_SDIO;
    HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

    hsd.Instance = SDIO;
    hsd.Init.ClockEdge = SDIO_CLOCK_EDGE_RISING;
    hsd.Init.ClockBypass = SDIO_CLOCK_BYPASS_DISABLE;
    hsd.Init.ClockPowerSave = SDIO_CLOCK_POWER_SAVE_DISABLE;
    hsd.Init.BusWide = SDIO_BUS_WIDE_1B;
    hsd.Init.HardwareFlowControl = SDIO_HARDWARE_FLOW_CONTROL_DISABLE;
    hsd.Init.ClockDiv = 0x04;
}

void MX_Gamepad_Init(void) {
    __HAL_RCC_GPIOE_CLK_ENABLE();
    
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    
    // External buttons (UP, DOWN, LEFT, RIGHT, A, B)
    GPIO_InitStruct.Pin = GPIO_PIN_7 | GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);

    // On-board buttons (K0, K1 for Start, Select)
    GPIO_InitStruct.Pin = GPIO_PIN_3 | GPIO_PIN_4;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);
}

void SysTick_Handler(void) {
    HAL_IncTick();
}

void Error_Handler(void) {
    while (1) {}
}
