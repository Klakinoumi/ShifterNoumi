/**
 * mode_flash.h — memorise le mode (H / SEQ) d'un branchement a l'autre.
 *
 * Page flash dediee (MODE_FLASH_PAGE_ADDR, 1 KB juste sous la page de
 * calibration, cf gx100_boot.h) : chaque bascule ajoute un enregistrement
 * de 16 bits a la suite des precedents ; la page n'est effacee que quand
 * elle est pleine (512 bascules). Page separee de calibration_flash pour
 * qu'une coupure pendant cet effacement ne puisse jamais perdre la
 * calibration — au pire le mode retombe sur H.
 *
 * Un enregistrement vaut MODE_FLASH_RECORD_H ou MODE_FLASH_RECORD_SEQ ;
 * toute autre valeur (programmation interrompue, contenu anterieur de la
 * page) est ignoree au chargement, et la sauvegarde suivante ecrit apres.
 */
#ifndef MODE_FLASH_H
#define MODE_FLASH_H

#include <stdbool.h>

#include "gear_position.h"
#include "gx100_boot.h"

#define MODE_FLASH_PAGE_ADDR    GX100_SETTINGS_PAGE
#define MODE_FLASH_PAGE_SIZE    1024U
#define MODE_FLASH_RECORD_COUNT (MODE_FLASH_PAGE_SIZE / 2U)

#define MODE_FLASH_RECORD_H   0x4848U /* "HH" */
#define MODE_FLASH_RECORD_SEQ 0x5353U /* "SS" */

/* Dernier mode enregistre (GEAR_MODE_H ou GEAR_MODE_SEQ), ou
 * GEAR_MODE_NONE si la page n'en contient aucun (premier demarrage). */
gear_mode_t mode_flash_load(void);

/* Enregistre mode (GEAR_MODE_H ou GEAR_MODE_SEQ). N'ecrit rien s'il est
 * deja le dernier enregistre. Retourne false si l'ecriture ou
 * l'effacement flash a echoue. */
bool mode_flash_save(gear_mode_t mode);

#endif /* MODE_FLASH_H */
