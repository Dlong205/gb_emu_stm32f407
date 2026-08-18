#include "diskio.h"
#include "stm32f4xx_hal.h"

extern SD_HandleTypeDef hsd;

DSTATUS disk_initialize(BYTE pdrv) {
    if (pdrv != 0) return STA_NOINIT;
    if (HAL_SD_GetCardState(&hsd) == HAL_SD_CARD_TRANSFER) return 0;
    HAL_Delay(100);
    for (uint8_t attempt = 0; attempt < 3; attempt++) {
        if (HAL_SD_Init(&hsd) == HAL_OK) return 0;
        HAL_Delay(200);
    }
    return STA_NOINIT;
}

DSTATUS disk_status(BYTE pdrv) {
    if (pdrv != 0) return STA_NOINIT;
    HAL_SD_CardStateTypeDef state = HAL_SD_GetCardState(&hsd);
    return (state == HAL_SD_CARD_TRANSFER) ? 0 : STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, DWORD sector, UINT count) {
    if (pdrv != 0) return RES_NOTRDY;
    for (uint8_t attempt = 0; attempt < 3; attempt++) {
        if (HAL_SD_ReadBlocks(&hsd, (uint8_t*)buff, sector, count, 5000) != HAL_OK) continue;
        uint8_t ok = 1;
        for (uint32_t i = 0; i < 1000; i++) {
            HAL_SD_CardStateTypeDef st = HAL_SD_GetCardState(&hsd);
            if (st == HAL_SD_CARD_TRANSFER) { ok = 1; break; }
            if (st == HAL_SD_CARD_ERROR) { ok = 0; break; }
        }
        if (ok) return RES_OK;
        HAL_Delay(10);
    }
    return RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, DWORD sector, UINT count) {
    if (pdrv != 0) return RES_NOTRDY;
    if (HAL_SD_WriteBlocks(&hsd, (uint8_t*)buff, sector, count, 5000) != HAL_OK) {
        return RES_ERROR;
    }
    while (HAL_SD_GetCardState(&hsd) != HAL_SD_CARD_TRANSFER) {
        if (HAL_SD_GetCardState(&hsd) == HAL_SD_CARD_ERROR) return RES_ERROR;
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    if (pdrv != 0) return RES_PARERR;
    HAL_SD_CardInfoTypeDef cardInfo;
    HAL_SD_GetCardInfo(&hsd, &cardInfo);

    switch (cmd) {
        case CTRL_SYNC:
            return RES_OK;
        case GET_SECTOR_COUNT:
            *(DWORD*)buff = cardInfo.BlockNbr;
            return RES_OK;
        case GET_SECTOR_SIZE:
            *(WORD*)buff = cardInfo.BlockSize;
            return RES_OK;
        case GET_BLOCK_SIZE:
            *(DWORD*)buff = 1;
            return RES_OK;
        default:
            return RES_PARERR;
    }
}

DWORD get_fattime(void) {
    return ((DWORD)(2024 - 1980) << 25) | ((DWORD)1 << 21) | ((DWORD)1 << 16);
}