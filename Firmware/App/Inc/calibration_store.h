/**
 * calibration_store.h — jeu de calibration par defaut + API de mise a jour.
 *
 * Portage de PCB/gear_calibration.json. Les valeurs par defaut ci-dessous
 * sont celles mesurees sur l'exemplaire de reference (session Phase 4/5,
 * voir PLAN.md) ; elles servent de secours tant qu'aucune calibration
 * utilisateur n'a ete ecrite (flash vierge) ou si la flash est corrompue.
 *
 * Persistance flash : implementee via calibration_flash.h (wear-leveling
 * par rotation de slots dans la derniere page flash du STM32F103C8T6).
 * calibration_store_load() lit la derniere calibration valide (sinon
 * defauts) ; calibration_store_commit() applique une calibration complete
 * en RAM et la sauvegarde en flash en une fois.
 */
#ifndef CALIBRATION_STORE_H
#define CALIBRATION_STORE_H

#include "gear_position.h"

/* Remplit *out avec le jeu de calibration par defaut (valeurs mesurees
 * Phase 4/5) et calcule neutral[]/scale[] (appelle
 * gear_position_compute_normalization en interne). A appeler au demarrage
 * avant toute autre operation si calibration_store_load() ne trouve rien
 * de valide en flash. */
void calibration_store_defaults(gear_calibration_t *out);

/* Charge la calibration active (flash si presente/valide, sinon defauts).
 * Retourne true si les valeurs viennent de la flash, false si defauts. */
bool calibration_store_load(gear_calibration_t *out);

/* Remplace les 9 points de *calibration, recalcule neutral/scale, puis
 * sauvegarde en flash en une seule ecriture (calibration_flash_save). A
 * appeler une fois a la fin d'une calibration complete (CalibrationMode
 * garde les points en RAM jusque-la). */
void calibration_store_commit(gear_calibration_t *calibration, const hall_sample_t positions[GEAR_POSITION_COUNT]);

#endif /* CALIBRATION_STORE_H */
