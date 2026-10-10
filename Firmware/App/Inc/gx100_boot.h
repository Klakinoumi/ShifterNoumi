#ifndef GX100_BOOT_H
#define GX100_BOOT_H

/*
 * Contract between the USB bootloader (Bootloader/), the application
 * (Firmware/) and the PC wizard (Wizard/gx100wiz/layout.py mirrors these
 * values; Wizard/tests/test_layout.py checks they stay in sync).
 *
 * Flash map (STM32F103C8, 64 KB, 1 KB pages):
 *
 *   0x08000000  bootloader        4 KB  pages 0-3 (one write-protection bit)
 *   0x08001000  application      ~58 KB
 *   0x0800F7F8  app trailer       8 B   {length, crc}, written last by an update
 *   0x0800F800  settings          1 KB  owned by the app (mode_flash.h), never
 *                                       touched by updates
 *   0x0800FC00  calibration       1 KB  owned by the app, never touched by updates
 *
 * The bootloader starts the application only if the trailer describes a
 * complete image whose CRC matches, so an interrupted update leaves the
 * device in the bootloader instead of running half an application.
 */

#define GX100_FLASH_BASE        0x08000000UL
#define GX100_FLASH_PAGE_SIZE   1024UL
#define GX100_BOOT_SIZE         0x1000UL
#define GX100_APP_BASE          (GX100_FLASH_BASE + GX100_BOOT_SIZE)
#define GX100_APP_END           0x0800F800UL   /* = GX100_SETTINGS_PAGE */
#define GX100_APP_TRAILER_ADDR  (GX100_APP_END - 8UL)
#define GX100_APP_MAX_SIZE      (GX100_APP_TRAILER_ADDR - GX100_APP_BASE)
#define GX100_SETTINGS_PAGE     0x0800F800UL   /* = MODE_FLASH_PAGE_ADDR */
#define GX100_CALIBRATION_PAGE  0x0800FC00UL   /* = CALIBRATION_FLASH_PAGE_ADDR */

/* Trailer: two little-endian words. crc = STM32 hardware CRC (CRC-32/MPEG-2
 * over 32-bit words) of the first `length` bytes of the app, length a
 * multiple of 4. */
#define GX100_TRAILER_LENGTH_OFFSET  0U
#define GX100_TRAILER_CRC_OFFSET     4U

/* App -> bootloader request: the app writes this value to BKP_DR1 and
 * resets. Backup registers survive a system reset but not a power cycle, so
 * unplugging always returns to the application. */
#define GX100_BKP_BOOT_MAGIC    0x424CU

/* Line the app accepts on its CDC port to reboot into the bootloader. */
#define GX100_CDC_ENTER_BOOTLOADER  "!GX100:ENTER_BOOTLOADER"
/* Line the app accepts to erase its calibration page and restart on the
 * built-in defaults (shifternoumi-setup reset-calibration). */
#define GX100_CDC_RESET_CALIBRATION "!GX100:RESET_CALIBRATION"

/* USB identity. VID is ST's (see docs/known-issues.md). */
#define GX100_USB_VID           0x0483U
#define GX100_USB_PID_APP       0x572CU
#define GX100_USB_PID_BOOT      0x572DU

/* ---- Bootloader protocol over HID: 64-byte OUT report (request), 64-byte
 * IN report (response). One response per request.
 *
 * Request:   [0] cmd  [1] seq  [2..3] 0  [4..] arguments
 * Response:  [0] cmd  [1] seq  [2] status  [3] 0  [4..] payload
 */
#define GX100_REPORT_SIZE       64U
#define GX100_PROTO_VERSION     1U
#define GX100_BOOT_VERSION_MAJOR 1U
#define GX100_BOOT_VERSION_MINOR 0U

#define GX100_CMD_INFO          0x01U  /* -> payload below */
#define GX100_CMD_ERASE         0x02U  /* invalidate the app, start an update */
#define GX100_CMD_WRITE         0x03U  /* [4..7] offset from APP_BASE, [8] len, [12..] data */
#define GX100_CMD_COMMIT        0x04U  /* [4..7] length, [8..11] crc -> check, write trailer */
#define GX100_CMD_BOOT          0x05U  /* respond, then reset into the app */

#define GX100_WRITE_DATA_OFFSET 12U
#define GX100_WRITE_MAX_LEN     48U    /* multiple of 4, fits in one report */

/* INFO payload offsets (in the response report). */
#define GX100_INFO_MAGIC        4U     /* "GXBL" */
#define GX100_INFO_PROTO        8U
#define GX100_INFO_VER_MAJOR    9U
#define GX100_INFO_VER_MINOR    10U
#define GX100_INFO_APP_VALID    11U
#define GX100_INFO_APP_BASE     12U    /* u32 */
#define GX100_INFO_APP_MAX      16U    /* u32 */
#define GX100_INFO_PAGE_SIZE    20U    /* u32 */
#define GX100_INFO_APP_LENGTH   24U    /* u32, from the trailer (0xFFFFFFFF if none) */
#define GX100_INFO_APP_CRC      28U    /* u32, from the trailer */
#define GX100_INFO_UID          32U    /* 12 bytes, chip unique ID */
#define GX100_INFO_FLASH_KB     44U    /* u16 */

#define GX100_STATUS_OK          0U
#define GX100_STATUS_BAD_CMD     1U
#define GX100_STATUS_BAD_ARG     2U
#define GX100_STATUS_FLASH_ERROR 3U
#define GX100_STATUS_CRC_MISMATCH 4U
#define GX100_STATUS_NO_SESSION  5U    /* WRITE/COMMIT without ERASE first */

#endif /* GX100_BOOT_H */
