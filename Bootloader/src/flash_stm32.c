/* boot_hw.h on the real chip: flash controller, CRC unit, unique ID. */
#include "boot_hw.h"

#include "gx100_boot.h"
#include "stm32f1xx.h"

#define UID_BASE_ADDR       0x1FFFF7E8UL
#define FLASH_SIZE_ADDR     0x1FFFF7E0UL

static int in_app_area(uint32_t addr, uint32_t len)
{
    return addr >= GX100_APP_BASE && addr < GX100_APP_END && len <= GX100_APP_END - addr;
}

static void flash_unlock(void)
{
    if (FLASH->CR & FLASH_CR_LOCK) {
        FLASH->KEYR = 0x45670123UL;
        FLASH->KEYR = 0xCDEF89ABUL;
    }
}

/* Waits for the operation to finish, returns -1 on programming or
 * write-protection error. */
static int flash_wait(void)
{
    while (FLASH->SR & FLASH_SR_BSY) {
    }
    uint32_t sr = FLASH->SR;
    FLASH->SR = FLASH_SR_EOP | FLASH_SR_PGERR | FLASH_SR_WRPRTERR;
    return (sr & (FLASH_SR_PGERR | FLASH_SR_WRPRTERR)) ? -1 : 0;
}

int boot_flash_erase_page(uint32_t addr)
{
    if (!in_app_area(addr, GX100_FLASH_PAGE_SIZE) || (addr % GX100_FLASH_PAGE_SIZE) != 0U) {
        return -1;
    }
    flash_unlock();
    FLASH->CR |= FLASH_CR_PER;
    FLASH->AR = addr;
    FLASH->CR |= FLASH_CR_STRT;
    int err = flash_wait();
    FLASH->CR &= ~FLASH_CR_PER;
    FLASH->CR |= FLASH_CR_LOCK;

    const uint32_t *p = (const uint32_t *)addr;
    for (uint32_t i = 0; i < GX100_FLASH_PAGE_SIZE / 4U; i++) {
        if (p[i] != 0xFFFFFFFFUL) {
            return -1;
        }
    }
    return err;
}

int boot_flash_write(uint32_t addr, const uint8_t *data, uint32_t len)
{
    if (!in_app_area(addr, len) || (addr & 3U) != 0U || (len & 3U) != 0U) {
        return -1;
    }
    int err = 0;
    flash_unlock();
    FLASH->CR |= FLASH_CR_PG;
    for (uint32_t i = 0; i < len && err == 0; i += 2U) {
        *(volatile uint16_t *)(addr + i) = (uint16_t)(data[i] | (data[i + 1U] << 8));
        err = flash_wait();
    }
    FLASH->CR &= ~FLASH_CR_PG;
    FLASH->CR |= FLASH_CR_LOCK;

    const uint8_t *p = (const uint8_t *)addr;
    for (uint32_t i = 0; i < len; i++) {
        if (p[i] != data[i]) {
            return -1;
        }
    }
    return err;
}

const uint8_t *boot_flash_ptr(uint32_t addr)
{
    return (const uint8_t *)addr;
}

uint32_t boot_crc32(const uint8_t *data, uint32_t len_words)
{
    RCC->AHBENR |= RCC_AHBENR_CRCEN;
    CRC->CR = CRC_CR_RESET;
    const uint32_t *w = (const uint32_t *)data;
    for (uint32_t i = 0; i < len_words; i++) {
        CRC->DR = w[i];
    }
    uint32_t crc = CRC->DR;
    RCC->AHBENR &= ~RCC_AHBENR_CRCEN;
    return crc;
}

void boot_device_uid(uint8_t out[12])
{
    const uint8_t *uid = (const uint8_t *)UID_BASE_ADDR;
    for (uint32_t i = 0; i < 12U; i++) {
        out[i] = uid[i];
    }
}

uint16_t boot_flash_size_kb(void)
{
    return *(const uint16_t *)FLASH_SIZE_ADDR;
}
