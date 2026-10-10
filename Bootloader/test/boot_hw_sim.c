/* boot_hw.h in RAM, with the flash rules that matter: erase = whole page
 * to 0xFF, programming a half-word that is not erased fails (PGERR). */
#include "boot_hw_sim.h"

#include "boot_hw.h"
#include "gx100_boot.h"

#include <string.h>

uint8_t sim_flash[SIM_FLASH_SIZE];

uint8_t *sim_at(uint32_t addr)
{
    return &sim_flash[addr - GX100_FLASH_BASE];
}

void sim_reset(void)
{
    memset(sim_flash, 0xFF, sizeof sim_flash);
    memset(sim_flash, 0xB0, GX100_BOOT_SIZE);
}

static int in_app_area(uint32_t addr, uint32_t len)
{
    return addr >= GX100_APP_BASE && addr < GX100_APP_END && len <= GX100_APP_END - addr;
}

int boot_flash_erase_page(uint32_t addr)
{
    if (!in_app_area(addr, GX100_FLASH_PAGE_SIZE) || (addr % GX100_FLASH_PAGE_SIZE) != 0U) {
        return -1;
    }
    memset(sim_at(addr), 0xFF, GX100_FLASH_PAGE_SIZE);
    return 0;
}

int boot_flash_write(uint32_t addr, const uint8_t *data, uint32_t len)
{
    if (!in_app_area(addr, len) || (addr & 3U) != 0U || (len & 3U) != 0U) {
        return -1;
    }
    uint8_t *p = sim_at(addr);
    for (uint32_t i = 0; i < len; i += 2U) {
        if (p[i] != 0xFF || p[i + 1U] != 0xFF) {
            return -1;
        }
        p[i] = data[i];
        p[i + 1U] = data[i + 1U];
    }
    return 0;
}

const uint8_t *boot_flash_ptr(uint32_t addr)
{
    return sim_at(addr);
}

uint32_t boot_crc32(const uint8_t *data, uint32_t len_words)
{
    uint32_t crc = 0xFFFFFFFFUL;
    for (uint32_t w = 0; w < len_words; w++) {
        const uint8_t *b = data + 4U * w;
        crc ^= (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
        for (int bit = 0; bit < 32; bit++) {
            crc = (crc & 0x80000000UL) ? (crc << 1) ^ 0x04C11DB7UL : (crc << 1);
        }
    }
    return crc;
}

uint32_t sim_crc32_mpeg2_bytes(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFUL;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= (uint32_t)data[i] << 24;
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80000000UL) ? (crc << 1) ^ 0x04C11DB7UL : (crc << 1);
        }
    }
    return crc;
}

void boot_device_uid(uint8_t out[12])
{
    static const uint8_t uid[12] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC};
    memcpy(out, uid, sizeof uid);
}

uint16_t boot_flash_size_kb(void)
{
    return 64;
}
