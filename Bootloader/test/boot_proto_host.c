/*
 * The bootloader protocol behind stdin/stdout: reads 64-byte requests,
 * writes 64-byte responses. Lets the Python wizard run its real update code
 * against the real boot_proto.c (Wizard/tests/test_update_flow.py).
 *
 *   boot_proto_host FLASH_FILE
 *
 * FLASH_FILE holds the simulated 64 KB flash; it is created erased if
 * missing and saved after every request, so a test can kill the process
 * mid-update ("power cut") and start it again.
 */
#include "boot_hw_sim.h"
#include "boot_proto.h"
#include "gx100_boot.h"

#include <stdio.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

static void load(const char *path)
{
    sim_reset();
    FILE *f = fopen(path, "rb");
    if (f) {
        size_t n = fread(sim_flash, 1, sizeof sim_flash, f);
        (void)n;
        fclose(f);
    }
}

static int save(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        return -1;
    }
    size_t n = fwrite(sim_flash, 1, sizeof sim_flash, f);
    fclose(f);
    return n == sizeof sim_flash ? 0 : -1;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s FLASH_FILE\n", argv[0]);
        return 2;
    }
#ifdef _WIN32
    _setmode(0, _O_BINARY);  /* stdin */
    _setmode(1, _O_BINARY);  /* stdout */
#endif
    load(argv[1]);
    boot_proto_init();

    uint8_t req[GX100_REPORT_SIZE];
    uint8_t resp[GX100_REPORT_SIZE];
    while (fread(req, 1, sizeof req, stdin) == sizeof req) {
        boot_proto_handle(req, resp);
        if (save(argv[1]) != 0) {
            return 1;
        }
        fwrite(resp, 1, sizeof resp, stdout);
        fflush(stdout);
        if (boot_proto_boot_requested()) {
            return 0;  /* the real device resets here */
        }
    }
    return 0;
}
