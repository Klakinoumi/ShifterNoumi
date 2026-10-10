#ifndef BOOT_PROTO_H
#define BOOT_PROTO_H

#include <stdint.h>

/* Reads the trailer at GX100_APP_TRAILER_ADDR and checks the image it
 * describes: sane length, stack pointer in RAM, reset vector inside the
 * image, CRC match. Nonzero if the application can be started. */
int boot_app_valid(void);

void boot_proto_init(void);

/* Handles one 64-byte request, fills the 64-byte response. */
void boot_proto_handle(const uint8_t *req, uint8_t *resp);

/* Set once a BOOT command has been answered: the caller resets after the
 * response has left. */
int boot_proto_boot_requested(void);

#endif /* BOOT_PROTO_H */
