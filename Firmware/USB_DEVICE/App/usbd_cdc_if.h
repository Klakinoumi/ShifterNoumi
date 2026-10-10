/**
 * usbd_cdc_if.h — interface CDC (port COM) du firmware composite HID+CDC.
 * Callbacks appeles par usbd_composite.c, meme role que le CDC_ItfTypeDef
 * standard ST (cf Diag/Gx100_DiagFirm/USB_DEVICE/App/usbd_cdc_if.h) mais
 * sans passer par USBD_CDC_ItfTypeDef/USBD_CDC_RegisterInterface (la classe
 * composite les appelle directement par leur nom, cf usbd_composite.c).
 */
#ifndef USBD_CDC_IF_H
#define USBD_CDC_IF_H

#include <stdint.h>

#include "usbd_cdc.h" /* CDC_SEND_ENCAPSULATED_COMMAND etc, CDC_DATA_FS_MAX_PACKET_SIZE */

#define APP_RX_DATA_SIZE CDC_DATA_FS_MAX_PACKET_SIZE
#define APP_TX_DATA_SIZE 256U

int8_t CDC_IF_Init(void);
int8_t CDC_IF_DeInit(void);
int8_t CDC_IF_Control(uint8_t cmd, uint8_t *pbuf, uint16_t length);
int8_t CDC_IF_Receive(uint8_t *pbuf, uint32_t *Len);

/* A appeler depuis main.c pour streamer une ligne de trace (Hall/etat) sur
 * le port COM. Non bloquant : renvoie USBD_BUSY si le transfert precedent
 * n'est pas termine (le buffer de trace du pas courant est alors juste
 * perdu, pas de file d'attente — comportement volontairement simple, la
 * trace CDC part a chaque pas lent (100ms) donc la perte d'une ligne occasionnelle
 * n'affecte pas le diagnostic). */
uint8_t CDC_IF_Transmit(uint8_t *buf, uint16_t len);

/* true si le buffer passe au dernier CDC_IF_Transmit() a fini d'etre lu par
 * l'IRQ USB (transmission terminee ou jamais commencee) — l'appelant doit
 * verifier ceci avant de reecrire dans un buffer transmis precedemment,
 * sinon l'IRQ USB peut lire des octets modifies entre-temps (corruption).
 * Voir debug_trace.c pour l'usage : le buffer de ligne CSV est statique et
 * reutilise a chaque pas, donc il faut attendre la fin de la transmission
 * precedente avant de le reecrire. */
int CDC_IF_TxReady(void);

#endif /* USBD_CDC_IF_H */
