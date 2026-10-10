#include "boot_proto.h"

#include "boot_hw.h"
#include "gx100_boot.h"

#define APP_PAGE_COUNT ((GX100_APP_END - GX100_APP_BASE) / GX100_FLASH_PAGE_SIZE)
#define RAM_START 0x20000000UL
#define RAM_END   0x20005000UL   /* 20 KB */

/* Pages erased since the last ERASE command: WRITE erases a page the first
 * time it touches it, so an update only erases what the new image uses. */
static uint64_t s_erased_pages;
static int s_session_open;
static int s_boot_requested;

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t trailer_word(uint32_t offset)
{
    return get_u32(boot_flash_ptr(GX100_APP_TRAILER_ADDR + offset));
}

int boot_app_valid(void)
{
    uint32_t length = trailer_word(GX100_TRAILER_LENGTH_OFFSET);
    if (length < 8U || length > GX100_APP_MAX_SIZE || (length & 3U) != 0U) {
        return 0;
    }
    const uint8_t *app = boot_flash_ptr(GX100_APP_BASE);
    uint32_t sp = get_u32(app);
    uint32_t pc = get_u32(app + 4);
    if (sp <= RAM_START || sp > RAM_END || (pc & 1U) == 0U ||
        pc < GX100_APP_BASE || pc >= GX100_APP_BASE + length) {
        return 0;
    }
    return boot_crc32(app, length / 4U) == trailer_word(GX100_TRAILER_CRC_OFFSET);
}

void boot_proto_init(void)
{
    s_erased_pages = 0;
    s_session_open = 0;
    s_boot_requested = 0;
}

int boot_proto_boot_requested(void)
{
    return s_boot_requested;
}

static int erase_page_once(uint32_t page_index)
{
    uint64_t bit = (uint64_t)1 << page_index;
    if (s_erased_pages & bit) {
        return 0;
    }
    if (boot_flash_erase_page(GX100_APP_BASE + page_index * GX100_FLASH_PAGE_SIZE) != 0) {
        return -1;
    }
    s_erased_pages |= bit;
    return 0;
}

static uint8_t cmd_info(uint8_t *resp)
{
    resp[GX100_INFO_MAGIC + 0] = 'G';
    resp[GX100_INFO_MAGIC + 1] = 'X';
    resp[GX100_INFO_MAGIC + 2] = 'B';
    resp[GX100_INFO_MAGIC + 3] = 'L';
    resp[GX100_INFO_PROTO] = GX100_PROTO_VERSION;
    resp[GX100_INFO_VER_MAJOR] = GX100_BOOT_VERSION_MAJOR;
    resp[GX100_INFO_VER_MINOR] = GX100_BOOT_VERSION_MINOR;
    resp[GX100_INFO_APP_VALID] = (uint8_t)(boot_app_valid() ? 1 : 0);
    put_u32(resp + GX100_INFO_APP_BASE, GX100_APP_BASE);
    put_u32(resp + GX100_INFO_APP_MAX, GX100_APP_MAX_SIZE);
    put_u32(resp + GX100_INFO_PAGE_SIZE, GX100_FLASH_PAGE_SIZE);
    put_u32(resp + GX100_INFO_APP_LENGTH, trailer_word(GX100_TRAILER_LENGTH_OFFSET));
    put_u32(resp + GX100_INFO_APP_CRC, trailer_word(GX100_TRAILER_CRC_OFFSET));
    boot_device_uid(resp + GX100_INFO_UID);
    uint16_t kb = boot_flash_size_kb();
    resp[GX100_INFO_FLASH_KB] = (uint8_t)kb;
    resp[GX100_INFO_FLASH_KB + 1] = (uint8_t)(kb >> 8);
    return GX100_STATUS_OK;
}

/* Erases the first app page (vector table) and the trailer page, so the
 * app is invalid from here until a successful COMMIT. */
static uint8_t cmd_erase(void)
{
    s_erased_pages = 0;
    s_session_open = 0;
    if (erase_page_once(0) != 0 || erase_page_once(APP_PAGE_COUNT - 1U) != 0) {
        return GX100_STATUS_FLASH_ERROR;
    }
    s_session_open = 1;
    return GX100_STATUS_OK;
}

static uint8_t cmd_write(const uint8_t *req)
{
    if (!s_session_open) {
        return GX100_STATUS_NO_SESSION;
    }
    uint32_t offset = get_u32(req + 4);
    uint32_t len = req[8];
    if (len == 0U || len > GX100_WRITE_MAX_LEN || (len & 3U) != 0U || (offset & 3U) != 0U ||
        offset >= GX100_APP_MAX_SIZE || len > GX100_APP_MAX_SIZE - offset) {
        return GX100_STATUS_BAD_ARG;
    }
    uint32_t first = offset / GX100_FLASH_PAGE_SIZE;
    uint32_t last = (offset + len - 1U) / GX100_FLASH_PAGE_SIZE;
    for (uint32_t page = first; page <= last; page++) {
        if (erase_page_once(page) != 0) {
            return GX100_STATUS_FLASH_ERROR;
        }
    }
    if (boot_flash_write(GX100_APP_BASE + offset, req + GX100_WRITE_DATA_OFFSET, len) != 0) {
        return GX100_STATUS_FLASH_ERROR;
    }
    return GX100_STATUS_OK;
}

static uint8_t cmd_commit(const uint8_t *req)
{
    if (!s_session_open) {
        return GX100_STATUS_NO_SESSION;
    }
    uint32_t length = get_u32(req + 4);
    uint32_t crc = get_u32(req + 8);
    if (length < 8U || length > GX100_APP_MAX_SIZE || (length & 3U) != 0U) {
        return GX100_STATUS_BAD_ARG;
    }
    if (boot_crc32(boot_flash_ptr(GX100_APP_BASE), length / 4U) != crc) {
        return GX100_STATUS_CRC_MISMATCH;
    }
    uint8_t trailer[8];
    put_u32(trailer + GX100_TRAILER_LENGTH_OFFSET, length);
    put_u32(trailer + GX100_TRAILER_CRC_OFFSET, crc);
    if (boot_flash_write(GX100_APP_TRAILER_ADDR, trailer, sizeof trailer) != 0) {
        return GX100_STATUS_FLASH_ERROR;
    }
    s_session_open = 0;
    /* CRC matched but the vector table may still be nonsense (wrong file):
     * report it rather than rebooting into the bootloader silently. */
    return boot_app_valid() ? GX100_STATUS_OK : GX100_STATUS_BAD_ARG;
}

void boot_proto_handle(const uint8_t *req, uint8_t *resp)
{
    for (uint32_t i = 0; i < GX100_REPORT_SIZE; i++) {
        resp[i] = 0;
    }
    resp[0] = req[0];
    resp[1] = req[1];

    uint8_t status;
    switch (req[0]) {
    case GX100_CMD_INFO:   status = cmd_info(resp); break;
    case GX100_CMD_ERASE:  status = cmd_erase(); break;
    case GX100_CMD_WRITE:  status = cmd_write(req); break;
    case GX100_CMD_COMMIT: status = cmd_commit(req); break;
    case GX100_CMD_BOOT:
        status = GX100_STATUS_OK;
        s_boot_requested = 1;
        break;
    default:
        status = GX100_STATUS_BAD_CMD;
        break;
    }
    resp[2] = status;
}
