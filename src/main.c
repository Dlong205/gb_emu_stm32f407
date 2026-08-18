#include "stm32f4xx_hal.h"
#include "ssd1306.h"
#include "ff.h"
#include "stdio.h"
#include "string.h"
#include "stdbool.h"

#define PEANUT_GB_HEADER_ONLY
#include "peanut_gb.h"

/* ===================== CẤU HÌNH CHUNG ===================== */
#define GB_LCD_W   160
#define GB_LCD_H   144
#define OLED_W     128
#define OLED_H     64
/* Đọc ROM theo khối 512B (1 sector). Đọc đa sector (16KB) bị lỗi trên thẻ này. */
#define ROM_CACHE_SIZE 512

/* ===================== GLOBAL ===================== */
I2C_HandleTypeDef hi2c1;
SSD1306_t oled;
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
static uint8_t lcd_frame[GB_LCD_W * GB_LCD_H];

static volatile uint32_t tick_ms;
static uint32_t last_fps_time, fps_count;
static char gb_title[17];
static char disp_buf[64];
static char rom_path[32];

/* ===================== PROTOTYPES ===================== */
void SystemClock_Config(void);
void MX_I2C1_Init(void);
void MX_SDIO_SD_Init(void);
uint8_t gb_rom_read_cb(struct gb_s *gb, const uint_fast32_t addr);
uint8_t gb_cart_ram_read_cb(struct gb_s *gb, const uint_fast32_t addr);
void gb_cart_ram_write_cb(struct gb_s *gb, const uint_fast32_t addr, const uint8_t val);
void gb_error_cb(struct gb_s *gb, const enum gb_error_e gb_err, const uint16_t addr);
void gb_lcd_draw_line_cb(struct gb_s *gb, const uint8_t *pixels, const uint_fast8_t line);
void OLED_Print(const char *str, uint8_t line);
FRESULT open_first_rom(void);
static uint32_t crc32_compute(void);
void render_to_oled(void);

/* ===================== MAIN ===================== */
int main(void) {
    HAL_Init();
    SystemClock_Config();
    MX_I2C1_Init();

    /* LED trên PA6 (dùng cho vòng lặp lỗi) */
    __HAL_RCC_GPIOA_CLK_ENABLE();
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin = GPIO_PIN_6;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    HAL_Delay(100);

    /* Khởi tạo OLED TRƯỚC để mọi lỗi đều hiển thị được */
    if (ssd1306_Init(&oled, &hi2c1, SSD1306_I2C_ADDR_3C, 128, 64) != HAL_OK) {
        ssd1306_Init(&oled, &hi2c1, SSD1306_I2C_ADDR_3D, 128, 64);
    }

    OLED_Print("GameBoy Emu", 0);
    OLED_Print("SD init...", 1);
    ssd1306_UpdateScreen(&oled);

    /* SD init với retry 3 lần, LED nháy mỗi lần thử */
    MX_SDIO_SD_Init();
    HAL_StatusTypeDef sd_status = HAL_OK;
    for (int attempt = 0; attempt < 3; attempt++) {
        sprintf(disp_buf, "SD init %u/3", attempt + 1);
        OLED_Print(disp_buf, 1);
        ssd1306_UpdateScreen(&oled);
        HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_6);
        sd_status = HAL_SD_Init(&hsd);
        if (sd_status == HAL_OK) break;
        HAL_Delay(500);
    }
    if (sd_status != HAL_OK) {
        sprintf(disp_buf, "SD init FAIL ERR=%lX", (unsigned long)hsd.ErrorCode);
        OLED_Print(disp_buf, 1);
        ssd1306_UpdateScreen(&oled);
        while (1) { HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_6); HAL_Delay(200); }
    }
    OLED_Print("SD init OK", 1);

    /* Mount với retry 3 lần */
    OLED_Print("Mount SD...", 2);
    FRESULT fres = FR_DISK_ERR;
    for (int attempt = 0; attempt < 3 && fres != FR_OK; attempt++) {
        fres = f_mount(&fs, "0:", 1);
        if (fres != FR_OK) HAL_Delay(500);
    }
    if (fres != FR_OK) {
        sprintf(disp_buf, "Mount FAIL r=%u", (unsigned)fres);
        OLED_Print(disp_buf, 2);
        ssd1306_UpdateScreen(&oled);
        while (1) { HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_6); HAL_Delay(200); }
    }
    OLED_Print("Mount OK", 2);

    OLED_Print("Open ROM...", 3);
    ssd1306_UpdateScreen(&oled);
    fres = open_first_rom();
    if (fres != FR_OK) {
        sprintf(disp_buf, "Open ROM FAIL %u", (unsigned)fres);
        OLED_Print(disp_buf, 3);
        ssd1306_UpdateScreen(&oled);
        while (1) { HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_6); HAL_Delay(200); }
    }
    OLED_Print("ROM OK", 3);

    /* CRC toàn file để đối chiếu với file trên PC */
    OLED_Print("CRC...", 3);
    ssd1306_UpdateScreen(&oled);
    uint32_t rom_crc = crc32_compute();
    sprintf(disp_buf, "CRC %08lX", (unsigned long)rom_crc);
    OLED_Print(disp_buf, 3);
    ssd1306_UpdateScreen(&oled);
    HAL_Delay(1500);

    /* Khởi tạo emulator. Các callback đọc ROM/CartRAM/... */
    enum gb_init_error_e gb_init_err =
        gb_init(&gb, gb_rom_read_cb, gb_cart_ram_read_cb,
                gb_cart_ram_write_cb, gb_error_cb, NULL);
    if (gb_init_err != GB_INIT_NO_ERROR) {
        /* Chẩn đoán: đọc lại header nhiều lần để kiểm tra độ ổn định */
        uint8_t ct_a = gb_rom_read_cb(&gb, 0x0147);
        uint8_t ct_b = gb_rom_read_cb(&gb, 0x0147);
        uint8_t bnk_a = gb_rom_read_cb(&gb, 0x0148);
        uint8_t ram_a = gb_rom_read_cb(&gb, 0x0149);
        uint8_t chk_expect = gb_rom_read_cb(&gb, 0x014D);
        uint8_t chk_calc = 0;
        for (uint16_t i = 0x134; i <= 0x14C; i++)
            chk_calc -= gb_rom_read_cb(&gb, i) + 1;
        char title[17];
        for (uint16_t i = 0; i < 16; i++) {
            char c = (char)gb_rom_read_cb(&gb, 0x134 + i);
            title[i] = (c >= ' ' && c <= '_') ? c : '.';
        }
        title[16] = 0;
        sprintf(disp_buf, "CRC %08lX", (unsigned long)rom_crc);
        OLED_Print(disp_buf, 2);
        OLED_Print(rom_path, 3);
        OLED_Print(title, 4);
        sprintf(disp_buf, "CT:%02X/%02X B:%02X R:%02X",
                ct_a, ct_b, bnk_a, ram_a);
        OLED_Print(disp_buf, 5);
        sprintf(disp_buf, "CHK %02X/%02X e=%u", chk_calc, chk_expect,
                (unsigned)gb_init_err);
        OLED_Print(disp_buf, 6);
        sprintf(disp_buf, "EP %02X%02X%02X SZ%lu",
                gb_rom_read_cb(&gb, 0x0100), gb_rom_read_cb(&gb, 0x0101),
                gb_rom_read_cb(&gb, 0x0102), (unsigned long)f_size(&rom_file));
        OLED_Print(disp_buf, 7);
        ssd1306_UpdateScreen(&oled);
        while (1) { HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_6); HAL_Delay(200); }
    }

    /* Cấp RAM cho lưu trữ (save) của game nếu có */
    size_t cart_ram_size = 0;
    gb_get_save_size_s(&gb, &cart_ram_size);
    if (cart_ram_size > 0x8000) cart_ram_size = 0x8000;

    /* Lấy tên game từ header ROM */
    gb_get_rom_name(&gb, gb_title);
    OLED_Print(gb_title, 4);

    /* Bật LCD render */
    gb_init_lcd(&gb, gb_lcd_draw_line_cb);

    sprintf(disp_buf, "MBC%d RAM%uK", gb.mbc,
            (unsigned)(gb.num_ram_banks * 8));
    OLED_Print(disp_buf, 5);
    OLED_Print("Running...", 6);
    ssd1306_UpdateScreen(&oled);
    HAL_Delay(2000);

    /* ===================== MAIN LOOP ===================== */
    last_fps_time = HAL_GetTick();
    uint8_t frameskip = 0;
    while (1) {
        gb_run_frame(&gb);
        fps_count++;

        /* Cập nhật FPS 1 lần/giây */
        if (HAL_GetTick() - last_fps_time >= 1000) {
            sprintf(disp_buf, "FPS:%lu", (unsigned long)fps_count);
            last_fps_time = HAL_GetTick();
            fps_count = 0;
        }

        if (++frameskip >= 2) {
            frameskip = 0;
            render_to_oled();
        }
    }
}

/* ===================== CRC32 TOÀN FILE (đối chiếu với PC) ===================== */
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

/* ===================== CALLBACK ROM (STREAM TỪ SD CARD) ===================== */
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

/* Nhận từng dòng pixel (160px) từ emulator, lưu vào frame buffer */
void gb_lcd_draw_line_cb(struct gb_s *gb, const uint8_t *pixels, const uint_fast8_t line) {
    if (line < GB_LCD_H)
        memcpy(&lcd_frame[line * GB_LCD_W], pixels, GB_LCD_W);
}

/* ===================== MỞ ROM TỪ SD ===================== */
/* Kiểm tra file có phải ROM GB hợp lệ không:
 * - 8 bytes đầu Nintendo logo (cố định ở mọi ROM chính hãng) tại 0x104
 * - dung lượng = bội số của 32KB */
static bool rom_header_valid(void) {
    static const uint8_t logo[8] = {0xCE, 0xED, 0x66, 0x66,
                                    0xCC, 0x0D, 0x00, 0x0B};
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

    /* Thử các tên quen thuộc trước */
    static const char *known[] = {
        "0:POKEMON_RED.GB", "0:POKEMON_RED.gb",
        "0:POKEMON_BLUE.GB", "0:POKEMON_BLUE.gb"
    };
    for (size_t i = 0; i < sizeof(known)/sizeof(known[0]); i++) {
        fres = f_open(&rom_file, known[i], FA_READ | FA_OPEN_EXISTING);
        if (fres == FR_OK) {
            if (rom_header_valid()) {
                strncpy(rom_path, known[i] + 2, sizeof(rom_path) - 1);
                rom_path[sizeof(rom_path) - 1] = 0;
                return FR_OK;
            }
            f_close(&rom_file);
        }
    }

    /* Nếu không, quét file .GB hoặc .gb và kiểm tra header từng file */
    DIR dir;
    FILINFO fno;
    if (f_findfirst(&dir, &fno, "0:", "*.GB") != FR_OK || fno.fname[0] == 0) {
        f_findfirst(&dir, &fno, "0:", "*.gb");
    }
    while (fno.fname[0] != 0) {
        sprintf(disp_buf, "0:%s", fno.fname);
        fres = f_open(&rom_file, disp_buf, FA_READ | FA_OPEN_EXISTING);
        if (fres == FR_OK) {
            if (rom_header_valid()) {
                strncpy(rom_path, fno.fname, sizeof(rom_path) - 1);
                rom_path[sizeof(rom_path) - 1] = 0;
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

/* ===================== RENDER GB FRAME → OLED 128x64 ===================== */
extern uint8_t SSD1306_Buffer[];

void render_to_oled(void) {
    static uint16_t sy_offsets[64];
    static bool offsets_calc = false;
    if (!offsets_calc) {
        for (int i = 0; i < 64; i++) {
            sy_offsets[i] = (i * 9 / 4) * GB_LCD_W;
        }
        offsets_calc = true;
    }

    uint16_t buf_idx = 0;
    for (uint8_t page = 0; page < 8; page++) {
        for (uint16_t ox = 0; ox < OLED_W; ox++) {
            uint16_t sx = ox * 5 / 4;
            uint8_t out_byte = 0;
            
            for (uint8_t i = 0; i < 8; i++) {
                uint16_t oy = page * 8 + i;
                uint8_t shade = lcd_frame[sy_offsets[oy] + sx] & 0x03;
                
                uint8_t pixel_on = 0;
                if (shade == 3) {
                    pixel_on = 1;
                } else if (shade == 2) {
                    pixel_on = ((ox + oy) & 1);
                } else if (shade == 1) {
                    pixel_on = (((ox + oy) & 1) == 0);
                }
                
                if (pixel_on) {
                    out_byte |= (1 << i);
                }
            }
            SSD1306_Buffer[buf_idx++] = out_byte;
        }
    }
    ssd1306_UpdateScreen(&oled);
}

/* ===================== OLED HELPERS ===================== */
void OLED_Print(const char *str, uint8_t line) {
    ssd1306_FillRectangle(&oled, 0, line * 8, OLED_W - 1, line * 8 + 7, Black);
    ssd1306_SetCursor(&oled, 0, line * 8);
    ssd1306_WriteString(&oled, (char*)str, Font_5x7, White);
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

void MX_I2C1_Init(void) {
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_I2C1_CLK_ENABLE();

    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin = GPIO_PIN_6 | GPIO_PIN_7;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_OD;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF4_I2C1;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    hi2c1.Instance = I2C1;
    hi2c1.Init.ClockSpeed = 400000;
    hi2c1.Init.DutyCycle = I2C_DUTYCYCLE_2;
    hi2c1.Init.OwnAddress1 = 0;
    hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
    hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    hi2c1.Init.OwnAddress2 = 0;
    hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
    HAL_I2C_Init(&hi2c1);
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

void SysTick_Handler(void) {
    HAL_IncTick();
}

void Error_Handler(void) {
    while (1) {}
}