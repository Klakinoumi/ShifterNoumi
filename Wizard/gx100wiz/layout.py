"""Flash map, USB identity and bootloader protocol.

Mirror of Firmware/App/Inc/gx100_boot.h (the source of truth);
tests/test_layout.py fails if the two drift apart.
"""

FLASH_BASE = 0x08000000
FLASH_PAGE_SIZE = 1024
BOOT_SIZE = 0x1000
APP_BASE = FLASH_BASE + BOOT_SIZE
APP_END = 0x0800F800                 # settings page starts here
APP_TRAILER_ADDR = APP_END - 8
APP_MAX_SIZE = APP_TRAILER_ADDR - APP_BASE
SETTINGS_PAGE = APP_END              # saved mode (mode_flash.h)
CALIBRATION_PAGE = 0x0800FC00
RAM_START = 0x20000000
RAM_END = 0x20005000

BKP_BOOT_MAGIC = 0x424C
CDC_ENTER_BOOTLOADER = "!GX100:ENTER_BOOTLOADER"
CDC_RESET_CALIBRATION = "!GX100:RESET_CALIBRATION"

USB_VID = 0x0483
USB_PID_APP = 0x572C
USB_PID_BOOT = 0x572D

# Built-in calibration (Firmware/App/Src/calibration_store.c), in the order
# of the debug port's CAL line: neutral, gear1..6, seq_up, seq_down; six
# sensors each (U2..U7). tests/test_layout.py checks it against the C file.
DEFAULT_CALIBRATION = [
    2977, 3093, 2917, 2979, 2772, 2902,
    3792, 3771, 2946, 3017, 2826, 2990,
    3012, 3109, 2980, 3771, 3375, 2938,
    3183, 4095, 3146, 3024, 2800, 2949,
    3020, 3078, 2973, 3188, 4046, 3067,
    3008, 3747, 3727, 3045, 2823, 2942,
    3062, 3103, 2942, 3008, 3538, 3664,
    2970, 3016, 2923, 3082, 3321, 2940,
    3037, 3942, 2998, 2991, 2789, 2924,
]

REPORT_SIZE = 64
PROTO_VERSION = 1

CMD_INFO = 0x01
CMD_ERASE = 0x02
CMD_WRITE = 0x03
CMD_COMMIT = 0x04
CMD_BOOT = 0x05

WRITE_DATA_OFFSET = 12
WRITE_MAX_LEN = 48

INFO_MAGIC = 4
INFO_PROTO = 8
INFO_VER_MAJOR = 9
INFO_VER_MINOR = 10
INFO_APP_VALID = 11
INFO_APP_BASE = 12
INFO_APP_MAX = 16
INFO_PAGE_SIZE = 20
INFO_APP_LENGTH = 24
INFO_APP_CRC = 28
INFO_UID = 32
INFO_FLASH_KB = 44

STATUS_OK = 0
STATUS_BAD_CMD = 1
STATUS_BAD_ARG = 2
STATUS_FLASH_ERROR = 3
STATUS_CRC_MISMATCH = 4
STATUS_NO_SESSION = 5

STATUS_TEXT = {
    STATUS_OK: "ok",
    STATUS_BAD_CMD: "unknown command",
    STATUS_BAD_ARG: "invalid argument",
    STATUS_FLASH_ERROR: "flash erase/write failed",
    STATUS_CRC_MISMATCH: "checksum mismatch after writing",
    STATUS_NO_SESSION: "write without erase",
}
