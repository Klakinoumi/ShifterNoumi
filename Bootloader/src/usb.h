#ifndef USB_H
#define USB_H

#include <stdint.h>

/* Minimal polled USB device: one vendor HID interface with 64-byte
 * interrupt IN and OUT reports. Needs a 48 MHz USB clock. */

void usb_init(void);
void usb_poll(void);

/* Copies the last OUT report into out[64] and returns 1, or returns 0 if
 * none is waiting. No new OUT report is accepted until usb_hid_send()'s
 * report has been read by the host (one response per request). */
int usb_hid_receive(uint8_t *out);

void usb_hid_send(const uint8_t *in);

/* 1 while a report given to usb_hid_send() has not been read yet. */
int usb_hid_tx_busy(void);

#endif /* USB_H */
