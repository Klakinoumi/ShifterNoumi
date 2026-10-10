#include "usbd_cdc_if.h"
#include "usbd_composite.h"
#include "boot_link.h"

extern USBD_HandleTypeDef hUsbDeviceFS;

static uint8_t CDC_IF_TxBuffer[APP_TX_DATA_SIZE];

int8_t CDC_IF_Init(void)
{
    USBD_COMPOSITE_CDC_SetTxBuffer(&hUsbDeviceFS, CDC_IF_TxBuffer, 0);
    return 0; /* USBD_OK */
}

int8_t CDC_IF_DeInit(void)
{
    return 0; /* USBD_OK */
}

int8_t CDC_IF_Control(uint8_t cmd, uint8_t *pbuf, uint16_t length)
{
    (void)pbuf;
    (void)length;
    /* Aucune commande CDC (line coding/control line state) n'a d'effet ici
     * — le port sert uniquement a streamer des donnees en sortie, comme le
     * firmware de diagnostic Phase 4 (cf usbd_cdc_if.c la-bas, meme choix). */
    switch (cmd)
    {
    default:
        break;
    }
    return 0; /* USBD_OK */
}

int8_t CDC_IF_Receive(uint8_t *Buf, uint32_t *Len)
{
    /* Commandes acceptees : passage en bootloader, remise a zero de la
     * calibration (cf boot_link.h). La
     * reception est re-armee par USBD_COMPOSITE_DataOut apres cet appel. */
    boot_link_on_cdc_rx(Buf, *Len);
    return 0; /* USBD_OK */
}

uint8_t CDC_IF_Transmit(uint8_t *buf, uint16_t len)
{
    if (hUsbDeviceFS.dev_state != USBD_STATE_CONFIGURED)
    {
        return (uint8_t)USBD_FAIL;
    }

    USBD_COMPOSITE_CDC_SetTxBuffer(&hUsbDeviceFS, buf, len);
    return USBD_COMPOSITE_CDC_TransmitPacket(&hUsbDeviceFS);
}

int CDC_IF_TxReady(void)
{
    return USBD_COMPOSITE_CDC_TxReady(&hUsbDeviceFS);
}
