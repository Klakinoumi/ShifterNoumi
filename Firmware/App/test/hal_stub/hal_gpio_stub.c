/*
 * hal_gpio_stub.c - stub HAL_GPIO_ReadPin/WritePin pour les bancs de test
 * hors cible qui linkent collar_switch.c ou status_led.c. Le niveau lu est
 * pilote par le test via hal_stub_gpio_level (meme niveau pour toutes les
 * broches) ; la derniere ecriture est gardee dans hal_stub_gpio_written.
 */
#include "main.h"

GPIO_PinState hal_stub_gpio_level = GPIO_PIN_RESET;

GPIO_PinState hal_stub_gpio_written = GPIO_PIN_SET;
int hal_stub_gpio_writes = 0;

GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *GPIOx, uint16_t GPIO_Pin)
{
    (void)GPIOx;
    (void)GPIO_Pin;
    return hal_stub_gpio_level;
}

void HAL_GPIO_WritePin(GPIO_TypeDef *GPIOx, uint16_t GPIO_Pin, GPIO_PinState PinState)
{
    (void)GPIOx;
    (void)GPIO_Pin;
    hal_stub_gpio_written = PinState;
    hal_stub_gpio_writes++;
}
