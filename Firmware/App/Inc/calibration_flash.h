/**
 * calibration_flash.h — persistance flash de gear_calibration_t.positions[]
 * (les seules 108 octets qui doivent survivre a un reboot ; modes[] est une
 * table fixe et neutral[]/scale[] sont derives, cf gear_position.h — ni l'un
 * ni l'autre n'a besoin d'etre stocke).
 *
 * Strategie de wear-leveling : la derniere page flash du STM32F103C8T6
 * (1 KB, 0x0800FC00-0x0800FFFF, la plus loin possible du code qui grandit
 * depuis le debut de la flash) est decoupee en plusieurs "slots" de taille
 * fixe (magic + numero de sequence + positions + CRC16). Une sauvegarde
 * ecrit le slot suivant (jamais un effacement) ; on n'efface la page que
 * lorsque tous les slots sont deja occupes, puis on repart au premier.
 * Au chargement, on scanne tous les slots valides (CRC OK) et on garde
 * celui au plus grand numero de sequence.
 *
 * Avec un usage occasionnel (calibration = geste utilisateur volontaire,
 * pas un cycle automatique), CALIBRATION_FLASH_SLOT_COUNT slots divisent
 * d'autant le nombre d'effacements de page sur la duree de vie du produit
 * (~10000 cycles d'effacement toleres par page en F1).
 *
 * Aucune dependance a mode_state_machine/gear_position au-dela de
 * hall_sample_t — module utilise par calibration_store.c, pas par le reste
 * de la logique metier.
 */
#ifndef CALIBRATION_FLASH_H
#define CALIBRATION_FLASH_H

#include <stdbool.h>
#include <stdint.h>

#include "gear_position.h"

/* Derniere page flash du STM32F103C8T6 (64 KB total, pages de 1 KB). */
#define CALIBRATION_FLASH_PAGE_ADDR 0x0800FC00UL
#define CALIBRATION_FLASH_PAGE_SIZE 1024U

/* positions[GEAR_POSITION_COUNT] = 9 * 12 = 108 octets ; slot arrondi a une
 * taille alignee 16 bits confortable pour magic/seq/CRC. */
#define CALIBRATION_FLASH_SLOT_SIZE   128U
#define CALIBRATION_FLASH_SLOT_COUNT  (CALIBRATION_FLASH_PAGE_SIZE / CALIBRATION_FLASH_SLOT_SIZE)

/* Charge la derniere calibration valide ecrite en flash (plus grand numero
 * de sequence parmi les slots dont le CRC est correct). Retourne true et
 * remplit *out_positions si un slot valide a ete trouve, false sinon
 * (flash vierge ou corrompue : l'appelant doit alors utiliser les
 * defauts). Ne modifie jamais la flash. */
bool calibration_flash_load(hall_sample_t out_positions[GEAR_POSITION_COUNT]);

/* Ecrit positions[] dans le slot suivant (numero de sequence courant + 1).
 * Efface la page au prealable si tous les slots sont deja occupes (le
 * dernier slot physique de la page a ete utilise), puis reecrit depuis le
 * premier slot avec le numero de sequence qui continue sa progression
 * (jamais remis a zero, pour que la logique "plus grand numero gagne"
 * reste valide meme juste apres un effacement). Retourne false si
 * l'ecriture/effacement flash a echoue (HAL_FLASH_*). */
bool calibration_flash_save(const hall_sample_t positions[GEAR_POSITION_COUNT]);

/* Efface la page de calibration : au prochain demarrage,
 * calibration_flash_load() ne trouve rien et le firmware repart sur les
 * defauts (commande GX100_CDC_RESET_CALIBRATION). Une coupure pendant
 * l'effacement donne le meme resultat (aucun slot au CRC valide).
 * Retourne false si l'effacement a echoue. */
bool calibration_flash_erase(void);

#endif /* CALIBRATION_FLASH_H */
