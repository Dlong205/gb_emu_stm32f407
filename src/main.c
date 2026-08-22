#include "stm32f4xx_hal.h"
#include "ili9341.h"
#include "ff.h"
#include "stdio.h"
#include "string.h"
#include "stdbool.h"

#define PEANUT_GB_HEADER_ONLY
#define ENABLE_SOUND 1
#include "peanut_gb.h"

#define MINIGB_APU_AUDIO_FORMAT_S16SYS
#include "minigb_apu.h"

/* ===================== CẤU HÌNH CHUNG ===================== */
#define GB_LCD_W   160
#define GB_LCD_H   144
#define ROM_CACHE_SIZE 512

/* ===================== GLOBAL ===================== */
SPI_HandleTypeDef hspi2;
DMA_HandleTypeDef hdma_spi2_tx;
DAC_HandleTypeDef hdac;
TIM_HandleTypeDef htim6;
DMA_HandleTypeDef hdma_dac1;
DMA_HandleTypeDef hdma_dac2;

FATFS fs;
SD_HandleTypeDef hsd;
struct gb_s gb;
struct minigb_apu_ctx apu_ctx;

// Audio buffers
#define AUDIO_BUFFER_SIZE (AUDIO_SAMPLES * 2) // L+R
static int16_t minigb_audio_buf[AUDIO_BUFFER_SIZE];
// Double buffer for DAC (12-bit unsigned)
static uint16_t dac_buf_l[AUDIO_SAMPLES * 2];
static uint16_t dac_buf_r[AUDIO_SAMPLES * 2];

static volatile uint8_t current_dac_half = 0;
static uint8_t last_dac_half = 0;

FIL rom_file;
static uint8_t rom_bank0[16384];
#define CACHE_LINES 64
static uint8_t rom_cache[CACHE_LINES][ROM_CACHE_SIZE];
static uint32_t rom_cache_sector[CACHE_LINES];
static uint32_t rom_cache_age[CACHE_LINES];
static uint32_t cache_time = 0;
static bool cache_init = false;

static uint8_t cart_ram[0x8000];
static size_t current_cart_ram_size = 0;
static uint8_t ram_is_dirty = 0;
static uint32_t last_save_time = 0;

static const uint16_t gb_palette[4] = {
    0xFFFF, // Trắng
    0xBDF7, // Xám nhạt
    0x632C, // Xám đậm
    0x0000  // Đen
};

static uint32_t fps_count;
static uint32_t last_fps_time;
static char gb_title[17];
static char disp_buf[64];
static char rom_path[32];

/* ===================== PROTOTYPES ===================== */
void SystemClock_Config(void);
void MX_SPI2_Init(void);
void MX_DAC_Init(void);
void MX_DMA_Init(void);
void MX_TIM6_Init(void);
void MX_SDIO_SD_Init(void);
void MX_Gamepad_Init(void);

uint8_t gb_rom_read_cb(struct gb_s *gb, const uint_fast32_t addr);
uint8_t gb_cart_ram_read_cb(struct gb_s *gb, const uint_fast32_t addr);
void gb_cart_ram_write_cb(struct gb_s *gb, const uint_fast32_t addr, const uint8_t val);
void gb_error_cb(struct gb_s *gb, const enum gb_error_e gb_err, const uint16_t addr);
void gb_lcd_draw_line_cb(struct gb_s *gb, const uint8_t *pixels, const uint_fast8_t line);
FRESULT open_first_rom(void);
static uint32_t crc32_compute(void);

uint8_t audio_read(const uint16_t addr);
void audio_write(const uint16_t addr, const uint8_t val);

/* ===================== MAIN ===================== */
int main(void) {
    HAL_Init();
    SystemClock_Config();
    MX_DMA_Init();
    MX_SPI2_Init();
    MX_DAC_Init();
    MX_TIM6_Init();

    /* LED trên PA6 */
    __HAL_RCC_GPIOA_CLK_ENABLE();
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin = GPIO_PIN_6;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    /* Chân điều khiển TFT trên GPIOB: PB10 (RST), PB12 (CS), PB14 (DC) */
    __HAL_RCC_GPIOB_CLK_ENABLE();
    GPIO_InitStruct.Pin = GPIO_PIN_10 | GPIO_PIN_12 | GPIO_PIN_14;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_12, GPIO_PIN_SET); // CS_HIGH

    HAL_Delay(100);

    /* Khởi tạo ILI9341 */
    ILI9341_Init(&hspi2);
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
    current_cart_ram_size = cart_ram_size;
    
    // Load file save
    if (current_cart_ram_size > 0) {
        char sav_path[32];
        strcpy(sav_path, rom_path);
        char *dot = strrchr(sav_path, '.');
        if (dot) strcpy(dot, ".sav");
        else strcat(sav_path, ".sav");
        
        FIL sav_file;
        if (f_open(&sav_file, sav_path, FA_READ | FA_OPEN_EXISTING) == FR_OK) {
            UINT br;
            f_read(&sav_file, cart_ram, current_cart_ram_size, &br);
            f_close(&sav_file);
        }
    }

    gb_init_lcd(&gb, gb_lcd_draw_line_cb);

    /* Audio Init */
    minigb_apu_audio_init(&apu_ctx);
    
    // Start DAC DMA
    HAL_DAC_Start_DMA(&hdac, DAC_CHANNEL_1, (uint32_t*)dac_buf_l, AUDIO_SAMPLES * 2, DAC_ALIGN_12B_R);
    HAL_DAC_Start_DMA(&hdac, DAC_CHANNEL_2, (uint32_t*)dac_buf_r, AUDIO_SAMPLES * 2, DAC_ALIGN_12B_R);
    HAL_TIM_Base_Start(&htim6);

    /* ===================== MAIN LOOP ===================== */
    last_fps_time = HAL_GetTick();
    while (1) {
        // Wait for audio DMA to finish a half buffer (sync pacing to exactly ~59.7 fps)
        while (last_dac_half == current_dac_half) {
            // Idle CPU
        }
        uint8_t target_half = current_dac_half;
        last_dac_half = current_dac_half;

        uint8_t joypad = 0xFF; // All released
        for (int r = 0; r < 4; r++) {
            // Đặt tất cả 4 Row (PE7-PE10) lên CAO
            HAL_GPIO_WritePin(GPIOE, GPIO_PIN_7 | GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10, GPIO_PIN_SET);
            // Đặt Row hiện tại xuống THẤP
            uint16_t row_pin = (GPIO_PIN_7 << r);
            HAL_GPIO_WritePin(GPIOE, row_pin, GPIO_PIN_RESET);
            
            // Đợi tín hiệu ổn định
            for(volatile int k=0; k<200; k++); 
            
            uint8_t c1 = (HAL_GPIO_ReadPin(GPIOE, GPIO_PIN_11) == GPIO_PIN_RESET);
            uint8_t c2 = (HAL_GPIO_ReadPin(GPIOE, GPIO_PIN_12) == GPIO_PIN_RESET);
            uint8_t c3 = (HAL_GPIO_ReadPin(GPIOE, GPIO_PIN_13) == GPIO_PIN_RESET);
            uint8_t c4 = (HAL_GPIO_ReadPin(GPIOE, GPIO_PIN_14) == GPIO_PIN_RESET);

            if (r == 0) { // R1
                if (c2) joypad &= ~JOYPAD_DOWN; // Phím 2 -> DOWN
                if (c4) joypad &= ~JOYPAD_START; // Phím A
            } else if (r == 1) { // R2
                if (c1) joypad &= ~JOYPAD_RIGHT; // Phím 4 -> RIGHT
                if (c3) joypad &= ~JOYPAD_LEFT; // Phím 6 -> LEFT
                if (c4) joypad &= ~JOYPAD_SELECT; // Phím B
            } else if (r == 2) { // R3
                if (c2) joypad &= ~JOYPAD_UP; // Phím 8 -> UP
                if (c4) joypad &= ~JOYPAD_A; // Phím C
            } else if (r == 3) { // R4
                if (c4) joypad &= ~JOYPAD_B; // Phím D
            }
        }
        // Trả lại trạng thái idle
        HAL_GPIO_WritePin(GPIOE, GPIO_PIN_7 | GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10, GPIO_PIN_SET);
        
        gb.direct.joypad = joypad;
        gb.direct.frame_skip = 1; // Bật Frame Skip 30 FPS để tránh nghẽn cổ chai vật lý của SPI 21MHz

        // Execute one full frame
        gb_run_frame(&gb);

        // Generate audio samples for this frame
        minigb_apu_audio_callback(&apu_ctx, minigb_audio_buf);
        
        // Copy audio to DAC DMA buffer
        uint16_t offset = target_half ? AUDIO_SAMPLES : 0;
        for (int i = 0; i < AUDIO_SAMPLES; i++) {
            dac_buf_l[offset + i] = (minigb_audio_buf[i * 2] + 32768) >> 4;
            dac_buf_r[offset + i] = (minigb_audio_buf[i * 2 + 1] + 32768) >> 4;
        }

        // Tự động lưu game sau khi ngừng viết 2 giây
        if (ram_is_dirty && (HAL_GetTick() - last_save_time > 2000)) {
            char sav_path[32];
            strcpy(sav_path, rom_path);
            char *dot = strrchr(sav_path, '.');
            if (dot) strcpy(dot, ".sav");
            else strcat(sav_path, ".sav");
            
            FIL sav_file;
            if (f_open(&sav_file, sav_path, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
                UINT bw;
                f_write(&sav_file, cart_ram, current_cart_ram_size, &bw);
                f_close(&sav_file);
            }
            ram_is_dirty = 0;
        }

        fps_count++;
        if (HAL_GetTick() - last_fps_time >= 1000) {
            last_fps_time = HAL_GetTick();
            fps_count = 0;
        }
    }
}

/* ===================== DMA CALLBACKS ===================== */
void HAL_DAC_ConvHalfCpltCallbackCh1(DAC_HandleTypeDef* hdac) {
    current_dac_half = 0;
}
void HAL_DAC_ConvCpltCallbackCh1(DAC_HandleTypeDef* hdac) {
    current_dac_half = 1;
}

/* ===================== APU CALLBACKS ===================== */
uint8_t audio_read(const uint16_t addr) {
    return minigb_apu_audio_read(&apu_ctx, addr);
}
void audio_write(const uint16_t addr, const uint8_t val) {
    minigb_apu_audio_write(&apu_ctx, addr, val);
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
        for (int j = 0; j < CACHE_LINES; j++) {
            rom_cache_sector[j] = 0xFFFFFFFF;
            rom_cache_age[j] = 0;
        }
        cache_init = true;
    }

    uint32_t sector = addr / ROM_CACHE_SIZE;
    
    // Tối ưu hoá cực mạnh: Ghi nhớ sector vừa truy cập (Hit rate > 99%)
    static uint32_t last_sector = 0xFFFFFFFF;
    static int last_index = 0;
    if (sector == last_sector) {
        return rom_cache[last_index][addr % ROM_CACHE_SIZE];
    }
    
    cache_time++;
    
    // 1. Tìm trong Cache (Fully Associative)
    for (int i = 0; i < CACHE_LINES; i++) {
        if (rom_cache_sector[i] == sector) {
            rom_cache_age[i] = cache_time; // Cập nhật tuổi (mới nhất)
            last_sector = sector;
            last_index = i;
            return rom_cache[i][addr % ROM_CACHE_SIZE];
        }
    }
    
    // 2. Cache Miss -> Tìm block cũ nhất để ghi đè (LRU)
    uint32_t oldest_age = 0xFFFFFFFF;
    int oldest_idx = 0;
    for (int i = 0; i < CACHE_LINES; i++) {
        if (rom_cache_age[i] < oldest_age) {
            oldest_age = rom_cache_age[i];
            oldest_idx = i;
        }
    }
    
    // 3. Đọc từ SD Card vào block cũ nhất
    UINT br = 0;
    if (f_lseek(&rom_file, sector * ROM_CACHE_SIZE) == FR_OK) {
        f_read(&rom_file, rom_cache[oldest_idx], ROM_CACHE_SIZE, &br);
    }
    rom_cache_sector[oldest_idx] = sector;
    rom_cache_age[oldest_idx] = cache_time;
    
    last_sector = sector;
    last_index = oldest_idx;
    
    return rom_cache[oldest_idx][addr % ROM_CACHE_SIZE];
}

uint8_t gb_cart_ram_read_cb(struct gb_s *gb, const uint_fast32_t addr) {
    if (addr < sizeof(cart_ram)) return cart_ram[addr];
    return 0xFF;
}

void gb_cart_ram_write_cb(struct gb_s *gb, const uint_fast32_t addr, const uint8_t val) {
    if (addr < sizeof(cart_ram)) {
        if (cart_ram[addr] != val) {
            cart_ram[addr] = val;
            ram_is_dirty = 1;
            last_save_time = HAL_GetTick();
        }
    }
}

void gb_error_cb(struct gb_s *gb, const enum gb_error_e gb_err, const uint16_t addr) {
    (void)gb; (void)gb_err; (void)addr;
}

static uint16_t dma_line_buf[2][GB_LCD_W];
static uint8_t dma_buf_idx = 0;

/* ===================== RENDER GB FRAME → ILI9341 ===================== */
void gb_lcd_draw_line_cb(struct gb_s *gb, const uint8_t *pixels, const uint_fast8_t line) {
    if (line >= GB_LCD_H) return;
    
    // 1. Chờ DMA của dòng trước đó chạy xong
    while (!spi_dma_cplt) {
        // Idle CPU
    }
    
    // 2. Đổ màu vào mảng đệm hiện tại
    for (uint8_t x = 0; x < GB_LCD_W; x++) {
        uint8_t color_idx = pixels[x] & 0x03;
        dma_line_buf[dma_buf_idx][x] = gb_palette[color_idx];
    }
    
    // 3. Set toạ độ vẽ (Blocking SPI - chỉ tốn vài micro-giây)
    ILI9341_SetWindow(80, 48 + line, 80 + GB_LCD_W - 1, 48 + line);
    
    // 4. Bắn mảng đệm ra màn hình bằng DMA (Không chặn CPU)
    ILI9341_WriteBuffer_DMA((uint8_t*)dma_line_buf[dma_buf_idx], GB_LCD_W * 2);
    
    // 5. Đảo mảng đệm (Double Buffering) để dòng sau dùng mảng kia
    dma_buf_idx ^= 1;
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
            if (rom_header_valid()) {
                strcpy(rom_path, known[i]);
                return FR_OK;
            }
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
                strcpy(rom_path, disp_buf);
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

void MX_DMA_Init(void) {
    __HAL_RCC_DMA1_CLK_ENABLE();
    
    /* DMA1_Stream4_IRQn interrupt configuration for SPI2_TX */
    HAL_NVIC_SetPriority(DMA1_Stream4_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(DMA1_Stream4_IRQn);
    
    /* DMA1_Stream5_IRQn interrupt configuration for DAC1 */
    HAL_NVIC_SetPriority(DMA1_Stream5_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(DMA1_Stream5_IRQn);
    
    /* DMA1_Stream6_IRQn interrupt configuration for DAC2 */
    HAL_NVIC_SetPriority(DMA1_Stream6_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(DMA1_Stream6_IRQn);
}

void DMA1_Stream4_IRQHandler(void) {
    HAL_DMA_IRQHandler(&hdma_spi2_tx);
}

void DMA1_Stream5_IRQHandler(void) {
    HAL_DMA_IRQHandler(&hdma_dac1);
}

void DMA1_Stream6_IRQHandler(void) {
    HAL_DMA_IRQHandler(&hdma_dac2);
}

void MX_SPI2_Init(void) {
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_SPI2_CLK_ENABLE();

    GPIO_InitTypeDef GPIO_InitStruct = {0};
    /* SPI2 GPIO Configuration
       PB13     ------> SPI2_SCK
       PB15     ------> SPI2_MOSI 
    */
    GPIO_InitStruct.Pin = GPIO_PIN_13 | GPIO_PIN_15;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF5_SPI2;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    hspi2.Instance = SPI2;
    hspi2.Init.Mode = SPI_MODE_MASTER;
    hspi2.Init.Direction = SPI_DIRECTION_2LINES;
    hspi2.Init.DataSize = SPI_DATASIZE_8BIT;
    hspi2.Init.CLKPolarity = SPI_POLARITY_LOW;
    hspi2.Init.CLKPhase = SPI_PHASE_1EDGE;
    hspi2.Init.NSS = SPI_NSS_SOFT;
    // PCLK1 (42MHz) / 2 = 21MHz SPI (Tốc độ tối đa trên SPI2)
    hspi2.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_2;
    hspi2.Init.FirstBit = SPI_FIRSTBIT_MSB;
    hspi2.Init.TIMode = SPI_TIMODE_DISABLE;
    hspi2.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    hspi2.Init.CRCPolynomial = 10;
    HAL_SPI_Init(&hspi2);
    
    /* SPI2 DMA Init */
    hdma_spi2_tx.Instance = DMA1_Stream4;
    hdma_spi2_tx.Init.Channel = DMA_CHANNEL_0;
    hdma_spi2_tx.Init.Direction = DMA_MEMORY_TO_PERIPH;
    hdma_spi2_tx.Init.PeriphInc = DMA_PINC_DISABLE;
    hdma_spi2_tx.Init.MemInc = DMA_MINC_ENABLE;
    hdma_spi2_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    hdma_spi2_tx.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
    hdma_spi2_tx.Init.Mode = DMA_NORMAL;
    hdma_spi2_tx.Init.Priority = DMA_PRIORITY_VERY_HIGH;
    hdma_spi2_tx.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
    HAL_DMA_Init(&hdma_spi2_tx);

    __HAL_LINKDMA(&hspi2, hdmatx, hdma_spi2_tx);
}

void MX_DAC_Init(void) {
    __HAL_RCC_DAC_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    // PA4 (DAC_OUT1), PA5 (DAC_OUT2)
    GPIO_InitStruct.Pin = GPIO_PIN_4 | GPIO_PIN_5;
    GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    hdac.Instance = DAC;
    HAL_DAC_Init(&hdac);

    DAC_ChannelConfTypeDef sConfig = {0};
    sConfig.DAC_Trigger = DAC_TRIGGER_T6_TRGO;
    sConfig.DAC_OutputBuffer = DAC_OUTPUTBUFFER_ENABLE;
    HAL_DAC_ConfigChannel(&hdac, &sConfig, DAC_CHANNEL_1);
    HAL_DAC_ConfigChannel(&hdac, &sConfig, DAC_CHANNEL_2);

    /* DMA DAC1 */
    hdma_dac1.Instance = DMA1_Stream5;
    hdma_dac1.Init.Channel = DMA_CHANNEL_7;
    hdma_dac1.Init.Direction = DMA_MEMORY_TO_PERIPH;
    hdma_dac1.Init.PeriphInc = DMA_PINC_DISABLE;
    hdma_dac1.Init.MemInc = DMA_MINC_ENABLE;
    hdma_dac1.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
    hdma_dac1.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;
    hdma_dac1.Init.Mode = DMA_CIRCULAR;
    hdma_dac1.Init.Priority = DMA_PRIORITY_HIGH;
    hdma_dac1.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
    HAL_DMA_Init(&hdma_dac1);
    __HAL_LINKDMA(&hdac, DMA_Handle1, hdma_dac1);

    /* DMA DAC2 */
    hdma_dac2.Instance = DMA1_Stream6;
    hdma_dac2.Init.Channel = DMA_CHANNEL_7;
    hdma_dac2.Init.Direction = DMA_MEMORY_TO_PERIPH;
    hdma_dac2.Init.PeriphInc = DMA_PINC_DISABLE;
    hdma_dac2.Init.MemInc = DMA_MINC_ENABLE;
    hdma_dac2.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
    hdma_dac2.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;
    hdma_dac2.Init.Mode = DMA_CIRCULAR;
    hdma_dac2.Init.Priority = DMA_PRIORITY_HIGH;
    hdma_dac2.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
    HAL_DMA_Init(&hdma_dac2);
    __HAL_LINKDMA(&hdac, DMA_Handle2, hdma_dac2);
}

void MX_TIM6_Init(void) {
    __HAL_RCC_TIM6_CLK_ENABLE();
    // APB1 timer clock is 84MHz. We need 32768Hz for audio samples.
    // 84000000 / 32768 = 2563.4
    // Prescaler = 0, Period = 2563 - 1
    htim6.Instance = TIM6;
    htim6.Init.Prescaler = 0;
    htim6.Init.CounterMode = TIM_COUNTERMODE_UP;
    htim6.Init.Period = 2562;
    htim6.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    HAL_TIM_Base_Init(&htim6);

    TIM_MasterConfigTypeDef sMasterConfig = {0};
    sMasterConfig.MasterOutputTrigger = TIM_TRGO_UPDATE;
    sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
    HAL_TIMEx_MasterConfigSynchronization(&htim6, &sMasterConfig);
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
    hsd.Init.ClockDiv = 0x00;
}

void MX_Gamepad_Init(void) {
    __HAL_RCC_GPIOE_CLK_ENABLE();
    
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    
    // 4 Chân Hàng (Row 1-4) -> Output
    GPIO_InitStruct.Pin = GPIO_PIN_7 | GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);
    // Đặt mặc định các Row ở mức CAO
    HAL_GPIO_WritePin(GPIOE, GPIO_PIN_7 | GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10, GPIO_PIN_SET);

    // 4 Chân Cột (Col 1-4) -> Input Pull-up
    GPIO_InitStruct.Pin = GPIO_PIN_11 | GPIO_PIN_12 | GPIO_PIN_13 | GPIO_PIN_14;
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
