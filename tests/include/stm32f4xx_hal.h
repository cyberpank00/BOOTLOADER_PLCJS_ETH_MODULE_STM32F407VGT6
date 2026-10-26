#ifndef TEST_HAL_H
#define TEST_HAL_H
#include <stdint.h>
typedef enum { HAL_OK, HAL_ERROR } HAL_StatusTypeDef;
typedef struct {
    uint32_t TypeErase, Banks, Sector, NbSectors, VoltageRange;
} FLASH_EraseInitTypeDef;
#define FLASH_TYPEERASE_SECTORS 0u
#define FLASH_BANK_1 1u
#define FLASH_VOLTAGE_RANGE_3 3u
#define FLASH_TYPEPROGRAM_WORD 2u
HAL_StatusTypeDef HAL_FLASH_Unlock(void);
HAL_StatusTypeDef HAL_FLASH_Lock(void);
HAL_StatusTypeDef HAL_FLASHEx_Erase(FLASH_EraseInitTypeDef *erase, uint32_t *error);
HAL_StatusTypeDef HAL_FLASH_Program(uint32_t type, uint32_t addr, uint64_t data);
uint32_t HAL_GetTick(void);
void HAL_NVIC_SystemReset(void);
#endif
