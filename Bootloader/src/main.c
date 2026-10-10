/*
 * GX100 USB bootloader.
 *
 * At reset (8 MHz HSI, nothing configured) it decides whether to start the
 * application. It stays in update mode if:
 *   1. the application asked for it (BKP_DR1 = GX100_BKP_BOOT_MAGIC), or
 *   2. no valid application is installed (trailer/CRC check), or
 *   3. the collar is lifted at power-up, released and lifted again within
 *      3 s. A collar merely held (or unplugged: PB1 then reads "lifted")
 *      starts the application normally.
 * Otherwise every register it touched is put back to its reset value and
 * it jumps to the application, which then configures the chip as usual.
 *
 * In update mode it runs at 48 MHz, blinks the LED and serves the HID
 * protocol described in gx100_boot.h.
 */
#include "boot_proto.h"
#include "gx100_boot.h"
#include "stm32f1xx.h"
#include "usb.h"

#define COLLAR_PIN       1U     /* PB1, reads 1 when lifted (pull-up) */
#define LED_PIN          13U    /* PC13, active low */
#define USB_DP_PIN       12U    /* PA12 */
#define GESTURE_WINDOW_MS 3000U
#define DEBOUNCE_MS      20U

static void systick_start(uint32_t core_hz)
{
    SysTick->LOAD = core_hz / 1000U - 1U;
    SysTick->VAL = 0;
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_ENABLE_Msk;
}

static void systick_stop(void)
{
    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL = 0;
}

/* 1 once per elapsed millisecond (COUNTFLAG clears on read). */
static int tick(void)
{
    return (SysTick->CTRL & SysTick_CTRL_COUNTFLAG_Msk) != 0U;
}

static void delay_ms(uint32_t ms)
{
    while (ms > 0U) {
        if (tick()) {
            ms--;
        }
    }
}

/* 4-bit CRL/CRH field of a GPIO pin. */
static void gpio_mode(GPIO_TypeDef *port, uint32_t pin, uint32_t mode)
{
    volatile uint32_t *reg = (pin < 8U) ? &port->CRL : &port->CRH;
    uint32_t shift = (pin & 7U) * 4U;
    *reg = (*reg & ~(0xFUL << shift)) | (mode << shift);
}
#define GPIO_IN_FLOATING 0x4U   /* reset state */
#define GPIO_IN_PULL     0x8U   /* pull direction from ODR */
#define GPIO_OUT_PP_2MHZ 0x2U

static void led_set(int on)
{
    if (on) {
        GPIOC->BRR = 1UL << LED_PIN;
    } else {
        GPIOC->BSRR = 1UL << LED_PIN;
    }
}

static int bkp_boot_request(void)
{
    RCC->APB1ENR |= RCC_APB1ENR_PWREN | RCC_APB1ENR_BKPEN;
    int requested = (BKP->DR1 == GX100_BKP_BOOT_MAGIC);
    if (requested) {
        PWR->CR |= PWR_CR_DBP;
        BKP->DR1 = 0;
        PWR->CR &= ~PWR_CR_DBP;
    }
    RCC->APB1ENR &= ~(RCC_APB1ENR_PWREN | RCC_APB1ENR_BKPEN);
    return requested;
}

/* Collar lifted at power-up -> wait (LED on) for a release then a new lift,
 * each stable for DEBOUNCE_MS, within GESTURE_WINDOW_MS. */
static int collar_gesture(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_IOPBEN | RCC_APB2ENR_IOPCEN;
    GPIOB->BSRR = 1UL << COLLAR_PIN;             /* pull-up */
    gpio_mode(GPIOB, COLLAR_PIN, GPIO_IN_PULL);
    systick_start(8000000U);                      /* HSI */
    delay_ms(5);

    int detected = 0;
    if (GPIOB->IDR & (1UL << COLLAR_PIN)) {
        GPIOC->BSRR = 1UL << LED_PIN;
        gpio_mode(GPIOC, LED_PIN, GPIO_OUT_PP_2MHZ);
        led_set(1);

        int want_lifted = 0;   /* first wait for the release */
        uint32_t stable = 0;
        for (uint32_t t = 0; t < GESTURE_WINDOW_MS && !detected;) {
            if (!tick()) {
                continue;
            }
            t++;
            int lifted = (GPIOB->IDR & (1UL << COLLAR_PIN)) != 0U;
            stable = (lifted == want_lifted) ? stable + 1U : 0U;
            if (stable >= DEBOUNCE_MS) {
                stable = 0;
                if (want_lifted) {
                    detected = 1;
                }
                want_lifted = !want_lifted;
            }
        }
        led_set(0);
        gpio_mode(GPIOC, LED_PIN, GPIO_IN_FLOATING);
        GPIOC->ODR = 0;
    }

    systick_stop();
    gpio_mode(GPIOB, COLLAR_PIN, GPIO_IN_FLOATING);
    GPIOB->ODR = 0;
    RCC->APB2ENR &= ~(RCC_APB2ENR_IOPBEN | RCC_APB2ENR_IOPCEN);
    return detected;
}

static void jump_to_app(void)
{
    const uint32_t *vectors = (const uint32_t *)GX100_APP_BASE;
    SCB->VTOR = GX100_APP_BASE;
    __asm volatile(
        "msr msp, %0\n"
        "bx %1\n"
        :
        : "r"(vectors[0]), "r"(vectors[1]));
    for (;;) {
    }
}

/* SYSCLK = USB clock = 48 MHz from the 8 MHz crystal (PLL x6). Falls back
 * to HSI/2 x12 if the crystal does not start (USB timing then out of spec,
 * but usually good enough to recover). */
static void clock_48mhz(void)
{
    RCC->CR |= RCC_CR_HSEON;
    uint32_t timeout = 200000U;
    while (!(RCC->CR & RCC_CR_HSERDY) && --timeout) {
    }
    uint32_t pll = (RCC->CR & RCC_CR_HSERDY) ? (RCC_CFGR_PLLSRC | RCC_CFGR_PLLMULL6) : RCC_CFGR_PLLMULL12;
    FLASH->ACR = FLASH_ACR_PRFTBE | 1U;           /* 1 wait state up to 48 MHz */
    RCC->CFGR = pll | RCC_CFGR_USBPRE | RCC_CFGR_PPRE1_DIV2;
    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY)) {
    }
    RCC->CFGR |= RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL) {
    }
}

/* The Blue Pill's D+ pull-up is hard-wired: holding D+ low makes the host
 * see a disconnect, so it enumerates us afresh after a reset. */
static void usb_force_reenumeration(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN;
    GPIOA->BRR = 1UL << USB_DP_PIN;
    gpio_mode(GPIOA, USB_DP_PIN, GPIO_OUT_PP_2MHZ);
    delay_ms(20);
    gpio_mode(GPIOA, USB_DP_PIN, GPIO_IN_FLOATING);
}

static void run_bootloader(void)
{
    clock_48mhz();
    systick_start(48000000U);

    RCC->APB2ENR |= RCC_APB2ENR_IOPCEN;
    gpio_mode(GPIOC, LED_PIN, GPIO_OUT_PP_2MHZ);

    usb_force_reenumeration();
    usb_init();
    boot_proto_init();

    static uint8_t req[GX100_REPORT_SIZE];
    static uint8_t resp[GX100_REPORT_SIZE];
    uint32_t ms = 0;
    uint32_t boot_wait_ms = 0;
    for (;;) {
        usb_poll();
        if (usb_hid_receive(req)) {
            boot_proto_handle(req, resp);
            usb_hid_send(resp);
        }
        if (!tick()) {
            continue;
        }
        ms++;
        led_set((ms / 100U) & 1U);  /* 5 Hz blink: "update mode" */

        /* After BOOT, reset once the host has read the response (or after
         * 500 ms if it never does). The app's own D+ pulse makes the host
         * enumerate it. */
        if (boot_proto_boot_requested()) {
            boot_wait_ms++;
            if (!usb_hid_tx_busy() || boot_wait_ms > 500U) {
                delay_ms(10);
                NVIC_SystemReset();
            }
        }
    }
}

int main(void)
{
    int stay = bkp_boot_request();
    if (!stay) {
        stay = !boot_app_valid();
    }
    if (!stay) {
        stay = collar_gesture();
    }
    if (!stay) {
        jump_to_app();
    }
    run_bootloader();
    return 0;
}
