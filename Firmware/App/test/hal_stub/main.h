/*
 * main.h - stub HAL minimal pour compiler les fichiers de App/Src hors cible dans les
 * bancs de test (App/test/). Ne remplace PAS le vrai Core/Inc/main.h du
 * firmware : uniquement les symboles necessaires aux modules testes ici.
 */
#ifndef MAIN_H
#define MAIN_H

#include <stdint.h>

typedef enum { HAL_OK = 0, HAL_ERROR = 1, HAL_BUSY = 2, HAL_TIMEOUT = 3 } HAL_StatusTypeDef;

#define FLASH_TYPEPROGRAM_HALFWORD 1U
#define FLASH_TYPEERASE_PAGES 0U

typedef struct {
    uint32_t TypeErase;
    uint32_t Banks;
    uint32_t PageAddress;
    uint32_t NbPages;
} FLASH_EraseInitTypeDef;

HAL_StatusTypeDef HAL_FLASH_Unlock(void);
HAL_StatusTypeDef HAL_FLASH_Lock(void);
HAL_StatusTypeDef HAL_FLASH_Program(uint32_t TypeProgram, uint32_t Address, uint64_t Data);
HAL_StatusTypeDef HAL_FLASHEx_Erase(FLASH_EraseInitTypeDef *pEraseInit, uint32_t *PageError);

/* GPIO (collar_switch.c) : HAL_GPIO_ReadPin est fournie par
 * hal_stub/hal_gpio_stub.c, qui renvoie hal_stub_gpio_level. */
typedef struct {
    uint32_t unused;
} GPIO_TypeDef;
typedef enum { GPIO_PIN_RESET = 0, GPIO_PIN_SET } GPIO_PinState;

#define GPIOB       ((GPIO_TypeDef *)0)
#define GPIOC       ((GPIO_TypeDef *)0)
#define GPIO_PIN_1  ((uint16_t)0x0002)
#define GPIO_PIN_13 ((uint16_t)0x2000)

extern GPIO_PinState hal_stub_gpio_level;
GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *GPIOx, uint16_t GPIO_Pin);
/* Ecriture (status_led.c) : derniere valeur ecrite + nombre d'ecritures. */
extern GPIO_PinState hal_stub_gpio_written;
extern int hal_stub_gpio_writes;
void HAL_GPIO_WritePin(GPIO_TypeDef *GPIOx, uint16_t GPIO_Pin, GPIO_PinState PinState);

#endif /* MAIN_H */
