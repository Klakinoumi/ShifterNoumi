/*
 * Register-level driver for the STM32F1 USB device peripheral (RM0008
 * chapter 23), polled from the main loop. EP0 = control, EP1 = interrupt
 * IN/OUT, 64 bytes each. Every descriptor fits in one 64-byte packet, so
 * control IN transfers are always a single packet.
 */
#include "usb.h"

#include "boot_hw.h"
#include "gx100_boot.h"
#include "stm32f1xx.h"

/* ---- Packet memory: 512 bytes seen by the USB core as 16-bit words, by the
 * CPU at 32-bit strides. Addresses below are USB-side byte offsets. */
#define PMA_BASE     0x40006000UL
#define EP0_RX_BUF   0x40U
#define EP0_TX_BUF   0x80U
#define EP1_RX_BUF   0xC0U
#define EP1_TX_BUF   0x100U
#define RX_COUNT_64  0x8400U   /* BL_SIZE=1 (32-byte blocks), NUM_BLOCK=1 -> 64 bytes */

#define PMA16(off)   (*(volatile uint32_t *)(PMA_BASE + 2U * (off)))
#define BT_ADDR_TX(ep)  PMA16((ep) * 8U + 0U)
#define BT_COUNT_TX(ep) PMA16((ep) * 8U + 2U)
#define BT_ADDR_RX(ep)  PMA16((ep) * 8U + 4U)
#define BT_COUNT_RX(ep) PMA16((ep) * 8U + 6U)

/* ---- Endpoint registers. STAT and DTOG bits toggle when written with 1,
 * CTR bits are cleared by writing 0: every write must be built with care. */
#define EPR(ep)      (*(volatile uint32_t *)(USB_BASE + 4U * (ep)))
#define EP_CTR_RX    0x8000U
#define EP_DTOG_RX   0x4000U
#define EP_STAT_RX   0x3000U
#define EP_SETUP     0x0800U
#define EP_TYPE_CTRL 0x0200U
#define EP_TYPE_INT  0x0600U
#define EP_CTR_TX    0x0080U
#define EP_DTOG_TX   0x0040U
#define EP_STAT_TX   0x0030U
#define EP_NOTOG     0x8F8FU   /* CTR_RX|SETUP|TYPE|KIND|CTR_TX|EA: plain bits */

#define RX_STALL 0x1000U
#define RX_NAK   0x2000U
#define RX_VALID 0x3000U
#define TX_STALL 0x0010U
#define TX_NAK   0x0020U
#define TX_VALID 0x0030U

static void ep_set_rx(uint32_t ep, uint32_t stat)
{
    uint32_t r = EPR(ep) & (EP_NOTOG | EP_STAT_RX);
    EPR(ep) = (r ^ stat) | EP_CTR_RX | EP_CTR_TX;
}

static void ep_set_tx(uint32_t ep, uint32_t stat)
{
    uint32_t r = EPR(ep) & (EP_NOTOG | EP_STAT_TX);
    EPR(ep) = (r ^ stat) | EP_CTR_RX | EP_CTR_TX;
}

/* type|address, both data toggles back to DATA0, both directions disabled. */
static void ep_setup(uint32_t ep, uint32_t type)
{
    uint32_t r = EPR(ep);
    EPR(ep) = type | ep | (r & (EP_DTOG_RX | EP_DTOG_TX | EP_STAT_RX | EP_STAT_TX));
}

static void pma_write(uint32_t off, const uint8_t *src, uint32_t len)
{
    for (uint32_t i = 0; i < len; i += 2U) {
        uint32_t hi = (i + 1U < len) ? src[i + 1U] : 0U;
        PMA16(off + i) = src[i] | (hi << 8);
    }
}

static void pma_read(uint32_t off, uint8_t *dst, uint32_t len)
{
    for (uint32_t i = 0; i < len; i += 2U) {
        uint32_t w = PMA16(off + i);
        dst[i] = (uint8_t)w;
        if (i + 1U < len) {
            dst[i + 1U] = (uint8_t)(w >> 8);
        }
    }
}

/* ---- Descriptors */
static const uint8_t k_report_desc[] = {
    0x06, 0x00, 0xFF,  /* Usage Page (Vendor 0xFF00) */
    0x09, 0x01,        /* Usage 1 */
    0xA1, 0x01,        /* Collection (Application) */
    0x15, 0x00,        /*   Logical Minimum 0 */
    0x26, 0xFF, 0x00,  /*   Logical Maximum 255 */
    0x75, 0x08,        /*   Report Size 8 */
    0x95, 0x40,        /*   Report Count 64 */
    0x09, 0x01,        /*   Usage 1 */
    0x81, 0x02,        /*   Input (Data, Var, Abs) */
    0x95, 0x40,        /*   Report Count 64 */
    0x09, 0x01,        /*   Usage 1 */
    0x91, 0x02,        /*   Output (Data, Var, Abs) */
    0xC0,              /* End Collection */
};

static const uint8_t k_device_desc[18] = {
    18, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 64,
    (uint8_t)GX100_USB_VID, (uint8_t)(GX100_USB_VID >> 8),
    (uint8_t)GX100_USB_PID_BOOT, (uint8_t)(GX100_USB_PID_BOOT >> 8),
    GX100_BOOT_VERSION_MINOR, GX100_BOOT_VERSION_MAJOR,
    1, 2, 3, 1,
};

#define HID_DESC_OFFSET 18U
static const uint8_t k_config_desc[41] = {
    9, 0x02, 41, 0, 1, 1, 0, 0x80, 50,                /* configuration, 100 mA */
    9, 0x04, 0, 0, 2, 0x03, 0x00, 0x00, 0,            /* interface: HID, no boot protocol */
    9, 0x21, 0x11, 0x01, 0x00, 1, 0x22, sizeof k_report_desc, 0,  /* HID 1.11 */
    7, 0x05, 0x81, 0x03, 64, 0, 1,                    /* EP1 IN, interrupt, 1 ms */
    7, 0x05, 0x01, 0x03, 64, 0, 1,                    /* EP1 OUT, interrupt, 1 ms */
};

static const char k_manufacturer[] = "ShifterNoumi";
static const char k_product[] = "ShifterNoumi Bootloader";

/* ---- State */
static uint8_t s_pending_address;
static uint8_t s_configured;
static uint8_t s_ep0_buf[64];
static uint8_t s_rx_report[GX100_REPORT_SIZE];
static volatile uint8_t s_rx_full;
static volatile uint8_t s_tx_busy;

static void ep0_send(const uint8_t *data, uint32_t len, uint32_t max_len)
{
    if (len > max_len) {
        len = max_len;
    }
    pma_write(EP0_TX_BUF, data, len);
    BT_COUNT_TX(0) = len;
    ep_set_tx(0, TX_VALID);
}

static void ep0_stall(void)
{
    ep_set_tx(0, TX_STALL);
    ep_set_rx(0, RX_STALL);
}

/* ASCII -> USB string descriptor in s_ep0_buf; returns its length. */
static uint32_t string_desc(const char *s)
{
    uint32_t n = 2;
    for (; *s != '\0' && n < sizeof s_ep0_buf; s++) {
        s_ep0_buf[n++] = (uint8_t)*s;
        s_ep0_buf[n++] = 0;
    }
    s_ep0_buf[0] = (uint8_t)n;
    s_ep0_buf[1] = 0x03;
    return n;
}

/* Serial number = the chip's unique ID in hex, so the wizard can tell two
 * shifters apart. */
static uint32_t serial_desc(void)
{
    static const char hex[] = "0123456789ABCDEF";
    uint8_t uid[12];
    char text[25];
    boot_device_uid(uid);
    for (uint32_t i = 0; i < 12U; i++) {
        text[2U * i] = hex[uid[i] >> 4];
        text[2U * i + 1U] = hex[uid[i] & 0x0FU];
    }
    text[24] = '\0';
    return string_desc(text);
}

static void handle_get_descriptor(uint32_t value, uint32_t max_len)
{
    uint32_t type = value >> 8;
    uint32_t index = value & 0xFFU;
    switch (type) {
    case 0x01:
        ep0_send(k_device_desc, sizeof k_device_desc, max_len);
        return;
    case 0x02:
        ep0_send(k_config_desc, sizeof k_config_desc, max_len);
        return;
    case 0x03:
        if (index == 0U) {
            static const uint8_t langid[4] = {4, 0x03, 0x09, 0x04};  /* en-US */
            ep0_send(langid, sizeof langid, max_len);
        } else if (index == 1U) {
            ep0_send(s_ep0_buf, string_desc(k_manufacturer), max_len);
        } else if (index == 2U) {
            ep0_send(s_ep0_buf, string_desc(k_product), max_len);
        } else if (index == 3U) {
            ep0_send(s_ep0_buf, serial_desc(), max_len);
        } else {
            ep0_stall();
        }
        return;
    case 0x21:
        ep0_send(k_config_desc + HID_DESC_OFFSET, 9, max_len);
        return;
    case 0x22:
        ep0_send(k_report_desc, sizeof k_report_desc, max_len);
        return;
    default:
        ep0_stall();  /* device qualifier, OS descriptors...: full-speed only */
        return;
    }
}

static void handle_setup(void)
{
    uint8_t setup[8];
    pma_read(EP0_RX_BUF, setup, 8);
    uint32_t request_type = setup[0];
    uint32_t request = setup[1];
    uint32_t value = setup[2] | ((uint32_t)setup[3] << 8);
    uint32_t length = setup[6] | ((uint32_t)setup[7] << 8);
    static const uint8_t zeros[2] = {0, 0};

    /* Ready for the status stage (OUT) of an IN transfer, or the next SETUP. */
    ep_set_rx(0, RX_VALID);

    if ((request_type & 0x60U) == 0x00U) {  /* standard */
        switch (request) {
        case 0x06:  /* GET_DESCRIPTOR */
            handle_get_descriptor(value, length);
            return;
        case 0x05:  /* SET_ADDRESS: applied once the status stage is sent */
            s_pending_address = (uint8_t)(value & 0x7FU);
            ep0_send(0, 0, 0);
            return;
        case 0x09:  /* SET_CONFIGURATION */
            s_configured = (uint8_t)value;
            if (s_configured) {
                ep_setup(1, EP_TYPE_INT);
                ep_set_rx(1, RX_VALID);
                ep_set_tx(1, TX_NAK);
                s_rx_full = 0;
                s_tx_busy = 0;
            }
            ep0_send(0, 0, 0);
            return;
        case 0x08:  /* GET_CONFIGURATION */
            ep0_send(&s_configured, 1, length);
            return;
        case 0x00:  /* GET_STATUS */
            ep0_send(zeros, 2, length);
            return;
        case 0x0A:  /* GET_INTERFACE */
            ep0_send(zeros, 1, length);
            return;
        case 0x01:  /* CLEAR_FEATURE */
        case 0x03:  /* SET_FEATURE */
        case 0x0B:  /* SET_INTERFACE */
            ep0_send(0, 0, 0);
            return;
        default:
            break;
        }
    } else if (request_type == 0x21U && (request == 0x0AU || request == 0x0BU)) {
        ep0_send(0, 0, 0);  /* HID SET_IDLE / SET_PROTOCOL: accepted, no effect */
        return;
    }
    ep0_stall();
}

static void bus_reset(void)
{
    USB->BTABLE = 0;
    BT_ADDR_TX(0) = EP0_TX_BUF;
    BT_COUNT_TX(0) = 0;
    BT_ADDR_RX(0) = EP0_RX_BUF;
    BT_COUNT_RX(0) = RX_COUNT_64;
    BT_ADDR_TX(1) = EP1_TX_BUF;
    BT_COUNT_TX(1) = 0;
    BT_ADDR_RX(1) = EP1_RX_BUF;
    BT_COUNT_RX(1) = RX_COUNT_64;

    ep_setup(0, EP_TYPE_CTRL);
    ep_set_rx(0, RX_VALID);
    ep_set_tx(0, TX_NAK);
    ep_setup(1, EP_TYPE_INT);  /* disabled until SET_CONFIGURATION */

    s_pending_address = 0;
    s_configured = 0;
    s_rx_full = 0;
    s_tx_busy = 0;
    USB->DADDR = USB_DADDR_EF;
}

void usb_init(void)
{
    RCC->APB1ENR |= RCC_APB1ENR_USBEN;
    USB->CNTR = USB_CNTR_FRES;          /* power up (PDWN=0), held in reset */
    for (volatile uint32_t i = 0; i < 100U; i++) {
    }                                   /* tSTARTUP: 1 us */
    USB->CNTR = 0;                      /* release reset; polled, no interrupts */
    USB->ISTR = 0;
}

void usb_poll(void)
{
    uint32_t istr = USB->ISTR;
    if (istr & USB_ISTR_RESET) {
        USB->ISTR = (uint16_t)~USB_ISTR_RESET;
        bus_reset();
        return;
    }
    /* Suspend, wakeup, SOF, errors: nothing to do but acknowledge. */
    if (istr & (USB_ISTR_SUSP | USB_ISTR_WKUP | USB_ISTR_SOF | USB_ISTR_ESOF |
                USB_ISTR_ERR | USB_ISTR_PMAOVR)) {
        USB->ISTR = (uint16_t)~(istr & (USB_ISTR_SUSP | USB_ISTR_WKUP | USB_ISTR_SOF |
                                        USB_ISTR_ESOF | USB_ISTR_ERR | USB_ISTR_PMAOVR));
    }

    while ((istr = USB->ISTR) & USB_ISTR_CTR) {
        uint32_t ep = istr & USB_ISTR_EP_ID;
        uint32_t r = EPR(ep);

        if (r & EP_CTR_RX) {
            EPR(ep) = (r & EP_NOTOG & ~EP_CTR_RX) | EP_CTR_TX;
            if (ep == 0U) {
                if (r & EP_SETUP) {
                    handle_setup();
                } else {
                    ep_set_rx(0, RX_VALID);  /* status stage of an IN transfer */
                }
            } else if (ep == 1U) {
                /* RX stays NAK (set by hardware) until the response is read. */
                pma_read(EP1_RX_BUF, s_rx_report, GX100_REPORT_SIZE);
                s_rx_full = 1;
            }
        }

        if (r & EP_CTR_TX) {
            EPR(ep) = (EPR(ep) & EP_NOTOG & ~EP_CTR_TX) | EP_CTR_RX;
            if (ep == 0U) {
                if (s_pending_address) {
                    USB->DADDR = USB_DADDR_EF | s_pending_address;
                    s_pending_address = 0;
                }
            } else if (ep == 1U) {
                s_tx_busy = 0;
                ep_set_rx(1, RX_VALID);
            }
        }
    }
}

int usb_hid_receive(uint8_t *out)
{
    if (!s_rx_full) {
        return 0;
    }
    for (uint32_t i = 0; i < GX100_REPORT_SIZE; i++) {
        out[i] = s_rx_report[i];
    }
    s_rx_full = 0;
    return 1;
}

void usb_hid_send(const uint8_t *in)
{
    pma_write(EP1_TX_BUF, in, GX100_REPORT_SIZE);
    BT_COUNT_TX(1) = GX100_REPORT_SIZE;
    s_tx_busy = 1;
    ep_set_tx(1, TX_VALID);
}

int usb_hid_tx_busy(void)
{
    return s_tx_busy;
}
