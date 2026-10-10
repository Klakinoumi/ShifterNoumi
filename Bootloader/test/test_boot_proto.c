/* Host tests of boot_proto.c against the RAM flash of boot_hw_sim.c.
 * Build and run: make -C Bootloader test */
#include "boot_hw.h"
#include "boot_hw_sim.h"
#include "boot_proto.h"
#include "gx100_boot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

static uint8_t g_seq;

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

/* Sends one request, returns the status byte; response copied to resp. */
static uint8_t request(uint8_t cmd, const uint8_t *args, uint32_t args_len, uint8_t *resp)
{
    uint8_t req[GX100_REPORT_SIZE] = {0};
    uint8_t local[GX100_REPORT_SIZE];
    req[0] = cmd;
    req[1] = ++g_seq;
    memcpy(req + 4, args, args_len);
    if (!resp) {
        resp = local;
    }
    boot_proto_handle(req, resp);
    if (resp[0] != cmd || resp[1] != g_seq) {
        printf("  FAIL response header mismatch\n");
        g_failures++;
    }
    return resp[2];
}

static uint8_t write_chunk(uint32_t offset, const uint8_t *data, uint32_t len)
{
    uint8_t args[GX100_REPORT_SIZE - 4] = {0};
    put_u32(args, offset);
    args[4] = (uint8_t)len;
    memcpy(args + (GX100_WRITE_DATA_OFFSET - 4), data, len);
    return request(GX100_CMD_WRITE, args, sizeof args, 0);
}

static uint8_t commit(uint32_t length, uint32_t crc)
{
    uint8_t args[8];
    put_u32(args, length);
    put_u32(args + 4, crc);
    return request(GX100_CMD_COMMIT, args, sizeof args, 0);
}

/* A plausible app image: SP at top of RAM, reset vector inside the image. */
static void make_image(uint8_t *img, uint32_t len, uint32_t seed)
{
    srand(seed);
    for (uint32_t i = 0; i < len; i++) {
        img[i] = (uint8_t)rand();
    }
    put_u32(img, 0x20005000UL);
    put_u32(img + 4, GX100_APP_BASE + 0x101U);
}

/* The full update sequence the wizard performs. Returns the COMMIT status. */
static uint8_t update(const uint8_t *img, uint32_t len)
{
    if (request(GX100_CMD_ERASE, 0, 0, 0) != GX100_STATUS_OK) {
        return 0xFF;
    }
    for (uint32_t off = 0; off < len; off += GX100_WRITE_MAX_LEN) {
        uint32_t n = len - off < GX100_WRITE_MAX_LEN ? len - off : GX100_WRITE_MAX_LEN;
        if (write_chunk(off, img + off, n) != GX100_STATUS_OK) {
            return 0xFE;
        }
    }
    return commit(len, boot_crc32(img, len / 4U));
}

static void test_crc_matches_catalogue(void)
{
    printf("crc: word CRC == CRC-32/MPEG-2 on byte-swapped words\n");
    CHECK(sim_crc32_mpeg2_bytes((const uint8_t *)"123456789", 9) == 0x0376E6E7UL);
    uint8_t le[8] = {0x78, 0x56, 0x34, 0x12, 0xEF, 0xCD, 0xAB, 0x89};
    uint8_t be[8] = {0x12, 0x34, 0x56, 0x78, 0x89, 0xAB, 0xCD, 0xEF};
    CHECK(boot_crc32(le, 2) == sim_crc32_mpeg2_bytes(be, 8));
}

static void test_blank_flash_is_invalid(void)
{
    printf("blank flash: no valid app\n");
    sim_reset();
    boot_proto_init();
    CHECK(!boot_app_valid());
    uint8_t resp[GX100_REPORT_SIZE];
    CHECK(request(GX100_CMD_INFO, 0, 0, resp) == GX100_STATUS_OK);
    CHECK(memcmp(resp + GX100_INFO_MAGIC, "GXBL", 4) == 0);
    CHECK(resp[GX100_INFO_APP_VALID] == 0);
    CHECK(get_u32(resp + GX100_INFO_APP_BASE) == GX100_APP_BASE);
    CHECK(get_u32(resp + GX100_INFO_APP_MAX) == GX100_APP_MAX_SIZE);
    CHECK(resp[GX100_INFO_UID] == 0x11);
}

static void test_full_update(void)
{
    printf("full update: valid app, INFO reports it, other areas untouched\n");
    sim_reset();
    memset(sim_at(GX100_SETTINGS_PAGE), 0x5A, GX100_FLASH_PAGE_SIZE);
    memset(sim_at(GX100_CALIBRATION_PAGE), 0x5A, GX100_FLASH_PAGE_SIZE);
    boot_proto_init();
    static uint8_t img[25796];
    make_image(img, sizeof img, 1);
    CHECK(update(img, sizeof img) == GX100_STATUS_OK);
    CHECK(boot_app_valid());
    CHECK(memcmp(sim_at(GX100_APP_BASE), img, sizeof img) == 0);

    uint8_t resp[GX100_REPORT_SIZE];
    request(GX100_CMD_INFO, 0, 0, resp);
    CHECK(resp[GX100_INFO_APP_VALID] == 1);
    CHECK(get_u32(resp + GX100_INFO_APP_LENGTH) == sizeof img);
    CHECK(get_u32(resp + GX100_INFO_APP_CRC) == boot_crc32(img, sizeof img / 4U));

    for (uint32_t i = 0; i < GX100_BOOT_SIZE; i++) {
        if (sim_flash[i] != 0xB0) {
            CHECK(!"bootloader area modified");
            break;
        }
    }
    for (uint32_t i = 0; i < 2U * GX100_FLASH_PAGE_SIZE; i++) {
        if (sim_at(GX100_SETTINGS_PAGE)[i] != 0x5A) {
            CHECK(!"settings or calibration page modified");
            break;
        }
    }
}

static void test_largest_image(void)
{
    printf("largest image: fills the app area up to the trailer\n");
    sim_reset();
    boot_proto_init();
    static uint8_t img[GX100_APP_MAX_SIZE];
    make_image(img, sizeof img, 2);
    CHECK(update(img, sizeof img) == GX100_STATUS_OK);
    CHECK(boot_app_valid());
}

static void test_update_over_existing_app(void)
{
    printf("second update: smaller image over a larger one\n");
    sim_reset();
    boot_proto_init();
    static uint8_t big[40000], small[9000];
    make_image(big, sizeof big, 3);
    make_image(small, sizeof small, 4);
    CHECK(update(big, sizeof big) == GX100_STATUS_OK);
    CHECK(update(small, sizeof small) == GX100_STATUS_OK);
    CHECK(boot_app_valid());
    CHECK(memcmp(sim_at(GX100_APP_BASE), small, sizeof small) == 0);
}

static void test_interrupted_update(void)
{
    printf("interrupted update: app invalid until COMMIT\n");
    sim_reset();
    boot_proto_init();
    static uint8_t img[20000];
    make_image(img, sizeof img, 5);
    CHECK(update(img, sizeof img) == GX100_STATUS_OK);

    CHECK(request(GX100_CMD_ERASE, 0, 0, 0) == GX100_STATUS_OK);
    CHECK(!boot_app_valid());
    CHECK(write_chunk(0, img, 48) == GX100_STATUS_OK);
    CHECK(!boot_app_valid());
    /* "Power cut": state lost, flash kept. Still invalid. */
    boot_proto_init();
    CHECK(!boot_app_valid());
    CHECK(write_chunk(48, img + 48, 48) == GX100_STATUS_NO_SESSION);
}

static void test_rejected_requests(void)
{
    printf("rejected requests: session, alignment, range, CRC, bad vectors\n");
    sim_reset();
    boot_proto_init();
    uint8_t data[48] = {0};
    CHECK(write_chunk(0, data, 48) == GX100_STATUS_NO_SESSION);
    CHECK(commit(48, 0) == GX100_STATUS_NO_SESSION);
    CHECK(request(0x7E, 0, 0, 0) == GX100_STATUS_BAD_CMD);

    CHECK(request(GX100_CMD_ERASE, 0, 0, 0) == GX100_STATUS_OK);
    CHECK(write_chunk(2, data, 48) == GX100_STATUS_BAD_ARG);           /* unaligned offset */
    CHECK(write_chunk(0, data, 6) == GX100_STATUS_BAD_ARG);            /* len not x4 */
    CHECK(write_chunk(0, data, 0) == GX100_STATUS_BAD_ARG);
    CHECK(write_chunk(GX100_APP_MAX_SIZE - 4U, data, 8) == GX100_STATUS_BAD_ARG);  /* over trailer */
    CHECK(write_chunk(GX100_APP_MAX_SIZE, data, 4) == GX100_STATUS_BAD_ARG);
    CHECK(write_chunk(0xFFFFFFF0UL, data, 16) == GX100_STATUS_BAD_ARG); /* wrap-around */
    CHECK(commit(GX100_APP_MAX_SIZE + 4U, 0) == GX100_STATUS_BAD_ARG);

    static uint8_t img[4096];
    make_image(img, sizeof img, 6);
    for (uint32_t off = 0; off < sizeof img; off += 48U) {
        uint32_t n = sizeof img - off < 48U ? sizeof img - off : 48U;
        write_chunk(off, img + off, n);
    }
    CHECK(commit(sizeof img, boot_crc32(img, sizeof img / 4U) ^ 1U) == GX100_STATUS_CRC_MISMATCH);
    CHECK(!boot_app_valid());

    /* CRC right, but the reset vector points outside the image. */
    sim_reset();
    boot_proto_init();
    put_u32(img + 4, GX100_APP_BASE + sizeof img + 0x101U);
    CHECK(update(img, sizeof img) == GX100_STATUS_BAD_ARG);
    CHECK(!boot_app_valid());
}

static void test_boot_command(void)
{
    printf("BOOT: answered, then flagged for reset\n");
    boot_proto_init();
    CHECK(!boot_proto_boot_requested());
    CHECK(request(GX100_CMD_BOOT, 0, 0, 0) == GX100_STATUS_OK);
    CHECK(boot_proto_boot_requested());
}

int main(void)
{
    test_crc_matches_catalogue();
    test_blank_flash_is_invalid();
    test_full_update();
    test_largest_image();
    test_update_over_existing_app();
    test_interrupted_update();
    test_rejected_requests();
    test_boot_command();
    printf(g_failures ? "%d FAILURE(S)\n" : "all tests passed\n", g_failures);
    return g_failures ? 1 : 0;
}
