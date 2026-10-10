#ifndef BOOT_LINK_H
#define BOOT_LINK_H

/*
 * boot_link — the application's side of the USB bootloader (gx100_boot.h),
 * and the other command the app accepts on its CDC port.
 * HAL adapter, like hall_sensors/collar_switch.
 *
 * - boot_link_usb_reenumerate(): holds D+ low for 20 ms before the USB
 *   stack starts. The Blue Pill's D+ pull-up is hard-wired, so without this
 *   the PC does not notice a reset (after an update, or after the bootloader
 *   hands over) and keeps talking to a device that no longer answers.
 * - boot_link_on_cdc_rx(): called from the CDC receive callback (USB
 *   interrupt); spots GX100_CDC_ENTER_BOOTLOADER and
 *   GX100_CDC_RESET_CALIBRATION.
 * - boot_link_poll(): from the main loop. Bootloader request: writes the
 *   backup-register magic and resets into the bootloader. Calibration
 *   reset: erases the calibration page (calibration_flash_erase) and
 *   restarts, so the app comes back on the built-in defaults. Does not
 *   return in either case.
 */

#include <stdint.h>

void boot_link_usb_reenumerate(void);
void boot_link_on_cdc_rx(const uint8_t *buf, uint32_t len);
void boot_link_poll(void);

#endif /* BOOT_LINK_H */
