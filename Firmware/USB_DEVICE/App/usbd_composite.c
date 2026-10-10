/**
 * usbd_composite.c — voir usbd_composite.h pour le contexte/la justification
 * de cette classe maison plutot que USBD_HID/USBD_CDC separees.
 *
 * Assemblage : la logique HID est une copie fonctionnelle directe de
 * Middlewares/.../HID/Src/usbd_hid.c (Init/DeInit/Setup/DataIn/SendReport),
 * la logique CDC une copie fonctionnelle directe de
 * Middlewares/.../CDC/Src/usbd_cdc.c (memes noms de champs / meme
 * sequence d'appels LL), toutes deux adaptees pour partager un seul
 * pClassData (usbd_composite_handle_t) et etre appelees depuis les
 * callbacks uniques Init/DeInit/Setup/DataIn/DataOut de USBD_Composite.
 * Aucune modification du comportement HID/CDC lui-meme par rapport aux
 * fichiers ST d'origine — uniquement leur assemblage.
 */
#include "usbd_composite.h"
#include "usbd_ctlreq.h"
#include "usbd_hid.h"   /* HID_MOUSE_ReportDesc, HID_MOUSE_REPORT_DESC_SIZE, HID_REQ_* */
#include "usbd_cdc.h"   /* CDC_IN_EP/CDC_OUT_EP/CDC_CMD_EP (deja decales, cf usbd_cdc.h GX100) */
#include "usbd_cdc_if.h"

#include <string.h>

/* --- Numeros d'interface --- */
#define ITF_HID       0U
#define ITF_CDC_COMM  1U
#define ITF_CDC_DATA  2U
#define COMPOSITE_NUM_INTERFACES 3U

typedef struct
{
    /* Etat HID, memes champs que USBD_HID_HandleTypeDef (usbd_hid.h). */
    uint32_t hid_protocol;
    uint32_t hid_idle_state;
    uint32_t hid_alt_setting;
    HID_StateTypeDef hid_state;

    /* Etat CDC, memes champs que USBD_CDC_HandleTypeDef (usbd_cdc.h). */
    uint32_t cdc_data[CDC_DATA_FS_MAX_PACKET_SIZE / 4U];
    uint8_t cdc_cmd_opcode;
    uint8_t cdc_cmd_length;
    uint8_t *cdc_rx_buffer;
    uint8_t *cdc_tx_buffer;
    uint32_t cdc_rx_length;
    uint32_t cdc_tx_length;
    __IO uint32_t cdc_tx_state;
    __IO uint32_t cdc_rx_state;
} usbd_composite_handle_t;

static uint8_t cdc_rx_buffer_storage[CDC_DATA_FS_MAX_PACKET_SIZE];

static uint8_t USBD_COMPOSITE_Init(USBD_HandleTypeDef *pdev, uint8_t cfgidx);
static uint8_t USBD_COMPOSITE_DeInit(USBD_HandleTypeDef *pdev, uint8_t cfgidx);
static uint8_t USBD_COMPOSITE_Setup(USBD_HandleTypeDef *pdev, USBD_SetupReqTypedef *req);
static uint8_t USBD_COMPOSITE_EP0_RxReady(USBD_HandleTypeDef *pdev);
static uint8_t USBD_COMPOSITE_DataIn(USBD_HandleTypeDef *pdev, uint8_t epnum);
static uint8_t USBD_COMPOSITE_DataOut(USBD_HandleTypeDef *pdev, uint8_t epnum);
static uint8_t *USBD_COMPOSITE_GetFSCfgDesc(uint16_t *length);
static uint8_t *USBD_COMPOSITE_GetDeviceQualifierDesc(uint16_t *length);

USBD_ClassTypeDef USBD_Composite = {
    USBD_COMPOSITE_Init,
    USBD_COMPOSITE_DeInit,
    USBD_COMPOSITE_Setup,
    NULL, /* EP0_TxSent */
    USBD_COMPOSITE_EP0_RxReady,
    USBD_COMPOSITE_DataIn,
    USBD_COMPOSITE_DataOut,
    NULL, /* SOF */
    NULL,
    NULL,
    USBD_COMPOSITE_GetFSCfgDesc, /* HS -> reutilise FS, device FS uniquement (F103) */
    USBD_COMPOSITE_GetFSCfgDesc,
    USBD_COMPOSITE_GetFSCfgDesc, /* OtherSpeed -> idem, jamais interroge en FS-only */
    USBD_COMPOSITE_GetDeviceQualifierDesc,
};

/* GX100 : report descriptor Gamepad d'origine (identique a
 * Middlewares/.../HID/Src/usbd_hid.c HID_MOUSE_ReportDesc) — duplique ici
 * plutot que reference car usbd_hid.c ne l'exporte pas (static). Si l'un
 * des deux est modifie, reappliquer le changement a l'autre. */
__ALIGN_BEGIN static uint8_t COMPOSITE_HID_ReportDesc[HID_MOUSE_REPORT_DESC_SIZE] __ALIGN_END = {
    0x05, 0x01,       /* Usage Page (Generic Desktop) */
    0x09, 0x05,       /* Usage (Gamepad) */
    0xA1, 0x01,       /* Collection (Application) */

    0x05, 0x09,       /*   Usage Page (Button) */
    0x19, 0x01,       /*   Usage Minimum (1) */
    0x29, 0x10,       /*   Usage Maximum (16) */
    0x15, 0x00,       /*   Logical Minimum (0) */
    0x25, 0x01,       /*   Logical Maximum (1) */
    0x75, 0x01,       /*   Report Size (1) */
    0x95, 0x10,       /*   Report Count (16) */
    0x81, 0x02,       /*   Input (Data,Var,Abs) : 2 octets boutons */

    0x05, 0x00,       /*   Usage Page (Undefined) */
    0x09, 0x01,       /*   Usage (Pointer) */
    0x15, 0x00,       /*   Logical Minimum (0) */
    0x25, 0xFF,       /*   Logical Maximum (255) */
    0x75, 0x08,       /*   Report Size (8) */
    0x95, 0x0E,       /*   Report Count (14) */
    0x81, 0x02,       /*   Input (Data,Var,Abs) : 14 octets vendor */

    0x09, 0x20,       /*   Usage (Vendor-defined) */
    0x15, 0x00,       /*   Logical Minimum (0) */
    0x26, 0xFF, 0x00, /*   Logical Maximum (255) */
    0x75, 0x08,       /*   Report Size (8) */
    0x95, 0x40,       /*   Report Count (64) */
    0x91, 0x02,       /*   Output (Data,Var,Abs) : 64 octets vendor */

    0xC0              /* End Collection */
};

__ALIGN_BEGIN static uint8_t COMPOSITE_HID_Desc[USB_HID_DESC_SIZ] __ALIGN_END = {
    0x09, HID_DESCRIPTOR_TYPE, 0x11, 0x01, 0x00, 0x01, 0x22,
    LOBYTE(HID_MOUSE_REPORT_DESC_SIZE), HIBYTE(HID_MOUSE_REPORT_DESC_SIZE),
};

__ALIGN_BEGIN static uint8_t USBD_COMPOSITE_DeviceQualifierDescBuf[USB_LEN_DEV_QUALIFIER_DESC] __ALIGN_END = {
    USB_LEN_DEV_QUALIFIER_DESC, USB_DESC_TYPE_DEVICE_QUALIFIER, 0x00, 0x02,
    0x00, 0x00, 0x00, 0x40, 0x01, 0x00,
};

/* Descriptor de configuration composite complet — voir usbd_composite.h
 * pour le detail des tailles de bloc (100 octets total). L'IAD (Interface
 * Association Descriptor) devant IF1/IF2 est ce qui fait que Windows
 * regroupe les deux interfaces CDC sous un seul port COM plutot que de les
 * traiter comme deux peripheriques independants (comportement standard
 * documente par Microsoft pour tout composite incluant du CDC-ACM). */
__ALIGN_BEGIN static uint8_t USBD_COMPOSITE_CfgFSDesc[USBD_COMPOSITE_CONFIG_DESC_SIZ] __ALIGN_END = {
    /* ---- Configuration Descriptor (9) ---- */
    0x09, USB_DESC_TYPE_CONFIGURATION,
    LOBYTE(USBD_COMPOSITE_CONFIG_DESC_SIZ), HIBYTE(USBD_COMPOSITE_CONFIG_DESC_SIZ),
    COMPOSITE_NUM_INTERFACES,
    0x01,       /* bConfigurationValue */
    0x00,       /* iConfiguration */
    0xC0,       /* bmAttributes: self powered */
    0x32,       /* MaxPower: 100mA */

    /* ---- Interface 0: HID Gamepad (9 + 9 + 7 = 25) ---- */
    0x09, USB_DESC_TYPE_INTERFACE,
    ITF_HID, 0x00,
    0x01,       /* bNumEndpoints */
    0x03,       /* bInterfaceClass: HID */
    0x00,       /* bInterfaceSubClass: no boot */
    0x00,       /* bInterfaceProtocol: none */
    0x00,

    0x09, HID_DESCRIPTOR_TYPE, 0x11, 0x01, 0x00, 0x01, 0x22,
    LOBYTE(HID_MOUSE_REPORT_DESC_SIZE), HIBYTE(HID_MOUSE_REPORT_DESC_SIZE),

    0x07, USB_DESC_TYPE_ENDPOINT,
    HID_EPIN_ADDR, 0x03,
    LOBYTE(HID_EPIN_SIZE), HIBYTE(HID_EPIN_SIZE),
    HID_FS_BINTERVAL,

    /* ---- IAD : regroupe IF1+IF2 comme une seule fonction CDC-ACM (8) ---- */
    0x08, 0x0B, /* bDescriptorType: Interface Association Descriptor */
    ITF_CDC_COMM,
    0x02,       /* bInterfaceCount: 2 (comm + data) */
    0x02,       /* bFunctionClass: CDC */
    0x02,       /* bFunctionSubClass: ACM */
    0x01,       /* bFunctionProtocol: AT commands */
    0x00,

    /* ---- Interface 1: CDC Communication (9+5+5+4+5+7 = 35) ---- */
    0x09, USB_DESC_TYPE_INTERFACE,
    ITF_CDC_COMM, 0x00,
    0x01,       /* bNumEndpoints */
    0x02,       /* bInterfaceClass: CDC Communication */
    0x02,       /* bInterfaceSubClass: ACM */
    0x01,       /* bInterfaceProtocol: AT commands */
    0x00,

    0x05, 0x24, 0x00, 0x10, 0x01,             /* Header Functional Desc */
    0x05, 0x24, 0x01, 0x00, ITF_CDC_DATA,     /* Call Management Func Desc */
    0x04, 0x24, 0x02, 0x02,                   /* ACM Functional Desc */
    0x05, 0x24, 0x06, ITF_CDC_COMM, ITF_CDC_DATA, /* Union Functional Desc */

    0x07, USB_DESC_TYPE_ENDPOINT,
    CDC_CMD_EP, 0x03,
    LOBYTE(CDC_CMD_PACKET_SIZE), HIBYTE(CDC_CMD_PACKET_SIZE),
    CDC_FS_BINTERVAL,

    /* ---- Interface 2: CDC Data (9+7+7 = 23) ---- */
    0x09, USB_DESC_TYPE_INTERFACE,
    ITF_CDC_DATA, 0x00,
    0x02,       /* bNumEndpoints */
    0x0A,       /* bInterfaceClass: CDC Data */
    0x00, 0x00, 0x00,

    0x07, USB_DESC_TYPE_ENDPOINT,
    CDC_OUT_EP, 0x02,
    LOBYTE(CDC_DATA_FS_MAX_PACKET_SIZE), HIBYTE(CDC_DATA_FS_MAX_PACKET_SIZE),
    0x00,

    0x07, USB_DESC_TYPE_ENDPOINT,
    CDC_IN_EP, 0x02,
    LOBYTE(CDC_DATA_FS_MAX_PACKET_SIZE), HIBYTE(CDC_DATA_FS_MAX_PACKET_SIZE),
    0x00,
};

/* ==================== Init / DeInit ==================== */

static uint8_t USBD_COMPOSITE_Init(USBD_HandleTypeDef *pdev, uint8_t cfgidx)
{
    UNUSED(cfgidx);

    /* --- HID endpoint --- */
    USBD_LL_OpenEP(pdev, HID_EPIN_ADDR, USBD_EP_TYPE_INTR, HID_EPIN_SIZE);
    pdev->ep_in[HID_EPIN_ADDR & 0xFU].is_used = 1U;

    /* --- CDC endpoints --- */
    USBD_LL_OpenEP(pdev, CDC_IN_EP, USBD_EP_TYPE_BULK, CDC_DATA_FS_IN_PACKET_SIZE);
    pdev->ep_in[CDC_IN_EP & 0xFU].is_used = 1U;
    USBD_LL_OpenEP(pdev, CDC_OUT_EP, USBD_EP_TYPE_BULK, CDC_DATA_FS_OUT_PACKET_SIZE);
    pdev->ep_out[CDC_OUT_EP & 0xFU].is_used = 1U;
    USBD_LL_OpenEP(pdev, CDC_CMD_EP, USBD_EP_TYPE_INTR, CDC_CMD_PACKET_SIZE);
    pdev->ep_in[CDC_CMD_EP & 0xFU].is_used = 1U;

    pdev->pClassData = USBD_malloc(sizeof(usbd_composite_handle_t));
    if (pdev->pClassData == NULL)
    {
        return (uint8_t)USBD_FAIL;
    }
    memset(pdev->pClassData, 0, sizeof(usbd_composite_handle_t));

    usbd_composite_handle_t *hcomp = (usbd_composite_handle_t *)pdev->pClassData;
    hcomp->hid_state = HID_IDLE;
    hcomp->cdc_rx_buffer = cdc_rx_buffer_storage;
    hcomp->cdc_cmd_opcode = 0xFFU;

    CDC_IF_Init();

    USBD_LL_PrepareReceive(pdev, CDC_OUT_EP, hcomp->cdc_rx_buffer, CDC_DATA_FS_OUT_PACKET_SIZE);

    return (uint8_t)USBD_OK;
}

static uint8_t USBD_COMPOSITE_DeInit(USBD_HandleTypeDef *pdev, uint8_t cfgidx)
{
    UNUSED(cfgidx);

    USBD_LL_CloseEP(pdev, HID_EPIN_ADDR);
    pdev->ep_in[HID_EPIN_ADDR & 0xFU].is_used = 0U;

    USBD_LL_CloseEP(pdev, CDC_IN_EP);
    pdev->ep_in[CDC_IN_EP & 0xFU].is_used = 0U;
    USBD_LL_CloseEP(pdev, CDC_OUT_EP);
    pdev->ep_out[CDC_OUT_EP & 0xFU].is_used = 0U;
    USBD_LL_CloseEP(pdev, CDC_CMD_EP);
    pdev->ep_in[CDC_CMD_EP & 0xFU].is_used = 0U;

    if (pdev->pClassData != NULL)
    {
        USBD_free(pdev->pClassData);
        pdev->pClassData = NULL;
    }

    return (uint8_t)USBD_OK;
}

/* ==================== Setup (dispatch par interface) ==================== */

static uint8_t USBD_COMPOSITE_Setup(USBD_HandleTypeDef *pdev, USBD_SetupReqTypedef *req)
{
    usbd_composite_handle_t *hcomp = (usbd_composite_handle_t *)pdev->pClassData;
    uint16_t len;
    uint8_t *pbuf;
    uint16_t status_info = 0U;
    uint8_t ifalt = 0U;
    USBD_StatusTypeDef ret = USBD_OK;
    uint8_t target_itf = LOBYTE(req->wIndex);

    switch (req->bmRequest & USB_REQ_TYPE_MASK)
    {
    case USB_REQ_TYPE_CLASS:
        if (target_itf == ITF_HID)
        {
            switch (req->bRequest)
            {
            case HID_REQ_SET_PROTOCOL:
                hcomp->hid_protocol = (uint8_t)(req->wValue);
                break;
            case HID_REQ_GET_PROTOCOL:
                USBD_CtlSendData(pdev, (uint8_t *)(void *)&hcomp->hid_protocol, 1U);
                break;
            case HID_REQ_SET_IDLE:
                hcomp->hid_idle_state = (uint8_t)(req->wValue >> 8);
                break;
            case HID_REQ_GET_IDLE:
                USBD_CtlSendData(pdev, (uint8_t *)(void *)&hcomp->hid_idle_state, 1U);
                break;
            default:
                USBD_CtlError(pdev, req);
                ret = USBD_FAIL;
                break;
            }
        }
        else
        {
            /* CDC (comm ou data interface) : suit usbd_cdc.c USBD_CDC_Setup
             * a l'identique, adapte au buffer combine. */
            if (req->wLength != 0U)
            {
                if ((req->bmRequest & 0x80U) != 0U)
                {
                    CDC_IF_Control(req->bRequest, (uint8_t *)(void *)hcomp->cdc_data, req->wLength);
                    USBD_CtlSendData(pdev, (uint8_t *)(void *)hcomp->cdc_data, req->wLength);
                }
                else
                {
                    hcomp->cdc_cmd_opcode = req->bRequest;
                    hcomp->cdc_cmd_length = (uint8_t)req->wLength;
                    USBD_CtlPrepareRx(pdev, (uint8_t *)(void *)hcomp->cdc_data, req->wLength);
                }
            }
            else
            {
                CDC_IF_Control(req->bRequest, (uint8_t *)(void *)req, 0U);
            }
        }
        break;

    case USB_REQ_TYPE_STANDARD:
        switch (req->bRequest)
        {
        case USB_REQ_GET_STATUS:
            if (pdev->dev_state == USBD_STATE_CONFIGURED)
            {
                USBD_CtlSendData(pdev, (uint8_t *)(void *)&status_info, 2U);
            }
            else
            {
                USBD_CtlError(pdev, req);
                ret = USBD_FAIL;
            }
            break;

        case USB_REQ_GET_DESCRIPTOR:
            if (target_itf == ITF_HID && (req->wValue >> 8) == HID_REPORT_DESC)
            {
                len = MIN(HID_MOUSE_REPORT_DESC_SIZE, req->wLength);
                pbuf = COMPOSITE_HID_ReportDesc;
                USBD_CtlSendData(pdev, pbuf, len);
            }
            else if (target_itf == ITF_HID && (req->wValue >> 8) == HID_DESCRIPTOR_TYPE)
            {
                len = MIN(USB_HID_DESC_SIZ, req->wLength);
                pbuf = COMPOSITE_HID_Desc;
                USBD_CtlSendData(pdev, pbuf, len);
            }
            else
            {
                USBD_CtlError(pdev, req);
                ret = USBD_FAIL;
            }
            break;

        case USB_REQ_GET_INTERFACE:
            if (pdev->dev_state == USBD_STATE_CONFIGURED)
            {
                USBD_CtlSendData(pdev, target_itf == ITF_HID ? (uint8_t *)(void *)&hcomp->hid_alt_setting : &ifalt,
                                  1U);
            }
            else
            {
                USBD_CtlError(pdev, req);
                ret = USBD_FAIL;
            }
            break;

        case USB_REQ_SET_INTERFACE:
            if (pdev->dev_state == USBD_STATE_CONFIGURED)
            {
                if (target_itf == ITF_HID)
                {
                    hcomp->hid_alt_setting = (uint8_t)(req->wValue);
                }
            }
            else
            {
                USBD_CtlError(pdev, req);
                ret = USBD_FAIL;
            }
            break;

        default:
            USBD_CtlError(pdev, req);
            ret = USBD_FAIL;
            break;
        }
        break;

    default:
        USBD_CtlError(pdev, req);
        ret = USBD_FAIL;
        break;
    }

    return (uint8_t)ret;
}

static uint8_t USBD_COMPOSITE_EP0_RxReady(USBD_HandleTypeDef *pdev)
{
    usbd_composite_handle_t *hcomp = (usbd_composite_handle_t *)pdev->pClassData;

    if (hcomp->cdc_cmd_opcode != 0xFFU)
    {
        CDC_IF_Control(hcomp->cdc_cmd_opcode, (uint8_t *)(void *)hcomp->cdc_data, (uint16_t)hcomp->cdc_cmd_length);
        hcomp->cdc_cmd_opcode = 0xFFU;
    }
    return (uint8_t)USBD_OK;
}

/* ==================== DataIn / DataOut (dispatch par endpoint) ==================== */

static uint8_t USBD_COMPOSITE_DataIn(USBD_HandleTypeDef *pdev, uint8_t epnum)
{
    usbd_composite_handle_t *hcomp = (usbd_composite_handle_t *)pdev->pClassData;

    if ((epnum | 0x80U) == HID_EPIN_ADDR)
    {
        hcomp->hid_state = HID_IDLE;
        return (uint8_t)USBD_OK;
    }

    if ((epnum | 0x80U) == CDC_IN_EP)
    {
        PCD_HandleTypeDef *hpcd = (PCD_HandleTypeDef *)pdev->pData;
        if ((pdev->ep_in[epnum].total_length > 0U) &&
            ((pdev->ep_in[epnum].total_length % hpcd->IN_ep[epnum].maxpacket) == 0U))
        {
            pdev->ep_in[epnum].total_length = 0U;
            /* GX100 : bug corrige — il faut l'adresse complete avec le bit
             * IN (CDC_IN_EP = 0x82), pas juste epnum (2) : USBD_LL_Transmit
             * -> HAL_PCD_EP_Transmit route par bit de direction, un epnum
             * sans ce bit visait l'endpoint OUT. */
            USBD_LL_Transmit(pdev, CDC_IN_EP, NULL, 0U);
        }
        else
        {
            hcomp->cdc_tx_state = 0U;
        }
        return (uint8_t)USBD_OK;
    }

    return (uint8_t)USBD_OK;
}

static uint8_t USBD_COMPOSITE_DataOut(USBD_HandleTypeDef *pdev, uint8_t epnum)
{
    usbd_composite_handle_t *hcomp = (usbd_composite_handle_t *)pdev->pClassData;

    if (epnum == (CDC_OUT_EP & 0x7FU))
    {
        hcomp->cdc_rx_length = USBD_LL_GetRxDataSize(pdev, epnum);
        CDC_IF_Receive(hcomp->cdc_rx_buffer, &hcomp->cdc_rx_length);
        /* GX100 : re-arme la reception, sinon l'endpoint reste en NAK apres
         * le premier paquet recu (commande bootloader, cf boot_link.h). */
        USBD_LL_PrepareReceive(pdev, CDC_OUT_EP, hcomp->cdc_rx_buffer, CDC_DATA_FS_OUT_PACKET_SIZE);
        return (uint8_t)USBD_OK;
    }

    return (uint8_t)USBD_OK;
}

/* ==================== Descriptors ==================== */

static uint8_t *USBD_COMPOSITE_GetFSCfgDesc(uint16_t *length)
{
    *length = sizeof(USBD_COMPOSITE_CfgFSDesc);
    return USBD_COMPOSITE_CfgFSDesc;
}

static uint8_t *USBD_COMPOSITE_GetDeviceQualifierDesc(uint16_t *length)
{
    *length = sizeof(USBD_COMPOSITE_DeviceQualifierDescBuf);
    return USBD_COMPOSITE_DeviceQualifierDescBuf;
}

/* ==================== API publique ==================== */

uint8_t USBD_COMPOSITE_HID_SendReport(USBD_HandleTypeDef *pdev, uint8_t *report, uint16_t len)
{
    usbd_composite_handle_t *hcomp = (usbd_composite_handle_t *)pdev->pClassData;

    if (pdev->dev_state == USBD_STATE_CONFIGURED)
    {
        if (hcomp->hid_state == HID_IDLE)
        {
            hcomp->hid_state = HID_BUSY;
            USBD_LL_Transmit(pdev, HID_EPIN_ADDR, report, len);
        }
    }
    return (uint8_t)USBD_OK;
}

uint8_t USBD_COMPOSITE_CDC_SetTxBuffer(USBD_HandleTypeDef *pdev, uint8_t *pbuff, uint16_t length)
{
    usbd_composite_handle_t *hcomp = (usbd_composite_handle_t *)pdev->pClassData;
    hcomp->cdc_tx_buffer = pbuff;
    hcomp->cdc_tx_length = length;
    return (uint8_t)USBD_OK;
}

uint8_t USBD_COMPOSITE_CDC_TransmitPacket(USBD_HandleTypeDef *pdev)
{
    usbd_composite_handle_t *hcomp = (usbd_composite_handle_t *)pdev->pClassData;

    if (hcomp->cdc_tx_state != 0U)
    {
        return (uint8_t)USBD_BUSY;
    }

    hcomp->cdc_tx_state = 1U;
    pdev->ep_in[CDC_IN_EP & 0xFU].total_length = hcomp->cdc_tx_length;
    USBD_LL_Transmit(pdev, CDC_IN_EP, hcomp->cdc_tx_buffer, (uint16_t)hcomp->cdc_tx_length);
    return (uint8_t)USBD_OK;
}

int USBD_COMPOSITE_CDC_TxReady(USBD_HandleTypeDef *pdev)
{
    usbd_composite_handle_t *hcomp = (usbd_composite_handle_t *)pdev->pClassData;
    if (hcomp == NULL)
    {
        return 1; /* pas encore initialise : rien en cours */
    }
    return hcomp->cdc_tx_state == 0U;
}
