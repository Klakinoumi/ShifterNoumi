/**
 * usbd_composite.h — classe USB device composite maison : HID gamepad
 * (interface 0, identique au report descriptor GX100 deja valide, cf
 * Middlewares/.../HID/Src/usbd_hid.c) + CDC (interfaces 1/2, port COM qui
 * streame les valeurs Hall brutes / etat interne pour le diagnostic).
 *
 * Pourquoi une classe maison plutot que USBD_HID + USBD_CDC telles quelles :
 * cette version de la lib USB device ST (STM32Cube_FW_F1_V1.8.7) n'a qu'un
 * seul pointeur de classe actif par device (USBD_HandleTypeDef.pClass, pas
 * un tableau) — CubeMX ne propose d'ailleurs pas de composite HID+CDC pour
 * l'IP USB device F1 dans son interface graphique (confirme en session).
 * Cette classe implemente donc USBD_ClassTypeDef une seule fois et
 * dispatche en interne vers la logique HID ou CDC selon l'endpoint/
 * l'interface concernee.
 *
 * Endpoints (FS, aucun conflit) :
 *   EP1 IN  (0x81) : HID gamepad (report Input 16 octets, cf hid_report.h)
 *   EP2 IN  (0x82) : CDC data IN  (host <- device)
 *   EP2 OUT (0x02) : CDC data OUT (host -> device, non utilise activement)
 *   EP3 IN  (0x83) : CDC notifications (interrupt, non utilise activement)
 * Interfaces :
 *   IF0 : HID (bInterfaceClass=0x03), descriptor report GX100 inchange
 *   IF1 : CDC Communication (ACM), IF2 : CDC Data — groupees par un IAD
 *   (Interface Association Descriptor) pour que Windows les traite comme
 *   un seul port COM au lieu de deux interfaces isolees.
 */
#ifndef USBD_COMPOSITE_H
#define USBD_COMPOSITE_H

#include "usbd_ioreq.h"

/* 9 (config header) + 25 (HID: IF+HIDdesc+EPIN) + 8 (IAD) +
 * 35 (CDC comm: IF+4 func descs+EP cmd) + 23 (CDC data: IF+EPOUT+EPIN) = 100 */
#define USBD_COMPOSITE_CONFIG_DESC_SIZ 100U

extern USBD_ClassTypeDef USBD_Composite;
#define USBD_COMPOSITE_CLASS &USBD_Composite

/* Taille (majoree) de la struct d'etat combinee HID+CDC allouee par
 * USBD_COMPOSITE_Init (pdev->pClassData). Utilisee par
 * USB_DEVICE/Target/usbd_conf.c (USBD_static_malloc) pour dimensionner le
 * buffer statique — cette lib USB device ST ne supporte qu'une seule
 * allocation globale (voir usbd_conf.c), il faut donc que ce buffer soit
 * au moins aussi grand que usbd_composite_handle_t (definie en prive dans
 * usbd_composite.c). Calcul : hid (4 champs 32 bits = 16o) + cdc_data
 * (CDC_DATA_FS_MAX_PACKET_SIZE = 64o) + 6 champs cdc scalaires (~24o) +
 * marge d'alignement/evolution. */
#define USBD_COMPOSITE_HANDLE_SIZE 128U

/* --- API HID (identique a l'usage existant de USBD_HID_SendReport) --- */
uint8_t USBD_COMPOSITE_HID_SendReport(USBD_HandleTypeDef *pdev, uint8_t *report, uint16_t len);

/* --- API CDC (miroir de usbd_cdc.h, meme forme d'appel pour reutiliser
 * le pattern deja connu du firmware de diagnostic Phase 4) --- */
uint8_t USBD_COMPOSITE_CDC_SetTxBuffer(USBD_HandleTypeDef *pdev, uint8_t *pbuff, uint16_t length);
uint8_t USBD_COMPOSITE_CDC_TransmitPacket(USBD_HandleTypeDef *pdev);

/* true si aucune transmission CDC n'est en cours (cdc_tx_state == 0) —
 * l'appelant doit verifier ceci avant de reecrire dans le buffer passe au
 * dernier SetTxBuffer/TransmitPacket, cf debug_trace.c (buffer de ligne
 * statique reutilise a chaque pas). */
int USBD_COMPOSITE_CDC_TxReady(USBD_HandleTypeDef *pdev);

#endif /* USBD_COMPOSITE_H */
