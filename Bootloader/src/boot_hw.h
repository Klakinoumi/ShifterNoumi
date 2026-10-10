#ifndef BOOT_HW_H
#define BOOT_HW_H

/*
 * What the protocol and the app check need from the hardware. Implemented
 * on the chip by flash_stm32.c, and in RAM by test/boot_hw_sim.c so the same
 * protocol code runs in host tests.
 */

#include <stdint.h>

/* Erase the 1 KB page starting at addr. Refuses (returns -1) anything
 * outside [GX100_APP_BASE, GX100_APP_END). 0 on success. */
int boot_flash_erase_page(uint32_t addr);

/* Program len bytes (addr and len multiples of 4) and read them back.
 * Same address restriction. 0 on success. */
int boot_flash_write(uint32_t addr, const uint8_t *data, uint32_t len);

/* Readable view of the flash at addr. */
const uint8_t *boot_flash_ptr(uint32_t addr);

/* STM32 CRC unit semantics: CRC-32 poly 0x04C11DB7, init 0xFFFFFFFF, fed
 * 32-bit words MSB first, no reflection, no final xor. */
uint32_t boot_crc32(const uint8_t *data, uint32_t len_words);

void boot_device_uid(uint8_t out[12]);
uint16_t boot_flash_size_kb(void);

#endif /* BOOT_HW_H */
