/**
 * debug_trace.h — trace de debug via le port CDC (2eme interface USB du
 * composite HID+CDC, cf USB_DEVICE/App/usbd_composite.c), en parallele du
 * rapport HID gamepad envoye sur la 1ere interface. Le gamepad continue de
 * fonctionner normalement en jeu ; le port COM streame en continu une ligne
 * CSV par pas lent (100ms), lisible par un script Python (pyserial), sur le
 * meme principe que le firmware de diagnostic Phase 4
 * (Diag/Gx100_DiagFirm, PCB/tools/hall_diag_stream.py) mais sans sacrifier
 * le device HID cette fois (composite au lieu de CDC seul).
 *
 * Objectif : observer en direct, sans reflash ni logiciel PC proprietaire,
 * ce que le firmware voit (valeurs Hall brutes, collerette) et decide
 * (position classifiee, mode courant, etat des detecteurs de geste/
 * calibration) pour diagnostiquer les boutons qui ne se declenchent pas et
 * l'absence de bascule de mode (cf PLAN.md Phase 6).
 *
 * Format CSV, un pas lent = une ligne (voir debug_trace_step pour le
 * detail des colonnes), terminee par \r\n. CDC_IF_Transmit (usbd_cdc_if.h)
 * est non bloquant : si le transfert precedent n'est pas termine (hote pas
 * en train de lire), la ligne du pas courant est silencieusement perdue —
 * pas de mise en file, pour ne jamais retarder la boucle principale/le HID.
 *
 * Aucune dependance au reste de App/ au-dela des types deja partages
 * (gear_position_t, mode_state_machine_t) : n'ecrit jamais dans ces
 * structures, lecture seule.
 */
#ifndef DEBUG_TRACE_H
#define DEBUG_TRACE_H

#include <stdint.h>

#include "mode_state_machine.h"

/* A appeler une fois au demarrage, avant le premier pas. Envoie une ligne
 * d'entete CSV (noms de colonnes) sur le port CDC. */
void debug_trace_init(void);

/* A appeler a chaque pas lent (100ms) de la boucle principale, apres
 * mode_state_machine_step() + hid_report_build() ET le pas rapide du meme
 * tour (fast_gear_step + hid_report_apply_fast_gear), pour que la ligne
 * decrive le rapport HID reellement envoye. Colonnes (CSV, separateur
 * virgule) :
 *   t_ms, u2, u3, u4, u5, u6, u7, collar,
 *   tracker_state, mode, report_debug,
 *   emit_pos, emit_kind,       (vide si aucun mouvement emis ce pas)
 *   switch_target,             (vide si aucune bascule appliquee ce pas)
 *   gear6_active, gear6_blips, seqdown_window, seqdown_count, seqdown_blips,
 *   calib_active, calib_step, calib_target,
 *   fast_pos,                  (position engagee par fast_gear, vide si aucune)
 *   buttons                    (octets 0-1 du rapport HID, uint16 decimal)
 * report est le rapport HID complet (hid_report.h) : report_debug =
 * octet HID_DEBUG_BYTE_INDEX, buttons = octets 0-1.
 */
void debug_trace_step(uint32_t t_ms, const hall_sample_t *hall, int collar, const mode_state_machine_t *msm,
                       const uint8_t *report, gear_position_t fast_pos);

/* A appeler a chaque tour de boucle. Envoie la calibration active sur une
 * ligne a part, des que le port est libre, toutes les
 * DEBUG_TRACE_CAL_PERIOD_MS et aussitot apres un changement (nouvelle
 * calibration appliquee, a la fin du mode calibration) :
 *   CAL,t_ms,<neutral u2..u7>,<gear1 u2..u7>,...,<seq_down u2..u7>
 * (positions dans l'ordre de gear_position_t, 54 valeurs). Permet de
 * rejouer une capture avec la calibration qu'avait le boitier (l'entete
 * n'est envoye qu'au demarrage, avant que l'hote n'ouvre le port). */
#define DEBUG_TRACE_CAL_PERIOD_MS 2000U
void debug_trace_calibration_poll(uint32_t t_ms, const gear_calibration_t *calibration);

#endif /* DEBUG_TRACE_H */
