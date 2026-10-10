#include "boot_link.h"

#include <string.h>

#include "calibration_flash.h"
#include "gx100_boot.h"
#include "main.h"

static volatile uint8_t s_enter_requested;
static volatile uint8_t s_reset_calibration_requested;

/* true si le paquet commence par la commande `cmd` (litteral). */
#define STARTS_WITH(buf, len, cmd) ((len) >= sizeof(cmd) - 1U && memcmp((buf), (cmd), sizeof(cmd) - 1U) == 0)

void boot_link_usb_reenumerate(void)
{
    GPIO_InitTypeDef gpio = {0};
    __HAL_RCC_GPIOA_CLK_ENABLE();
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_12, GPIO_PIN_RESET);
    gpio.Pin = GPIO_PIN_12;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOA, &gpio);
    HAL_Delay(20);
    HAL_GPIO_DeInit(GPIOA, GPIO_PIN_12);  /* back to input: the USB peripheral takes over */
}

void boot_link_on_cdc_rx(const uint8_t *buf, uint32_t len)
{
    /* The command must start the packet: a terminal or a modem prober
     * (Linux ModemManager sends "AT..." to new serial ports) cannot
     * trigger it by accident. */
    if (STARTS_WITH(buf, len, GX100_CDC_ENTER_BOOTLOADER)) {
        s_enter_requested = 1;
    } else if (STARTS_WITH(buf, len, GX100_CDC_RESET_CALIBRATION)) {
        s_reset_calibration_requested = 1;
    }
}

void boot_link_poll(void)
{
    if (s_reset_calibration_requested) {
        /* Let the USB acknowledge finish, erase, restart: the app loads
         * the defaults since the page is blank. */
        HAL_Delay(20);
        (void)calibration_flash_erase();
        NVIC_SystemReset();
    }
    if (!s_enter_requested) {
        return;
    }
    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_RCC_BKP_CLK_ENABLE();
    HAL_PWR_EnableBkUpAccess();
    BKP->DR1 = GX100_BKP_BOOT_MAGIC;
    HAL_PWR_DisableBkUpAccess();
    /* Let the USB acknowledge of the command finish before the reset. */
    HAL_Delay(20);
    NVIC_SystemReset();
}
