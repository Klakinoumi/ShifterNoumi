#ifndef BOOT_HW_SIM_H
#define BOOT_HW_SIM_H

#include <stdint.h>

/* The whole 64 KB flash, in RAM. Starts erased (0xFF) except the
 * bootloader area, filled with a recognizable pattern. */
#define SIM_FLASH_SIZE 0x10000UL
extern uint8_t sim_flash[SIM_FLASH_SIZE];

void sim_reset(void);
uint8_t *sim_at(uint32_t addr);

/* Byte-oriented CRC-32/MPEG-2, independent from boot_crc32(): used by the
 * tests to check the word-oriented implementation. */
uint32_t sim_crc32_mpeg2_bytes(const uint8_t *data, uint32_t len);

#endif /* BOOT_HW_SIM_H */
