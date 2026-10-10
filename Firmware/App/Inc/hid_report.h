/**
 * hid_report.h — traduction de l'etat logique (mode_state_machine_t) vers
 * le rapport HID Input transmis au PC.
 *
 * Le report descriptor reproduit la structure d'origine confirmee en
 * Phase 3 (PCB/hid_phase3/hid_decode.py) : 16 boutons (2 octets bitmask)
 * + 14 octets vendor-defined "Pointer", soit 16 octets au total.
 *
 * GX100 (2026-09-26, session debug "shift rapide 3<->4" en jeu — Assetto
 * Corsa) : les 14 octets vendor sont desormais peuples avec les 6 valeurs
 * Hall brutes (comme le firmware d'origine le faisait — confirme par
 * relecture de PCB/hid_phase3/hid_summary.txt, dont CHAQUE trame, y compris
 * au neutre, porte des valeurs ~2800-3800 non nulles dans ces octets,
 * exactement la plage des capteurs Hall du projet). Precedemment ces octets
 * restaient a 0 (seule la position deja classifiee etait exposee via les
 * boutons). Hypothese motivant ce changement : la comparaison directe
 * HID<->telemetrie AC (capture_session.py, session "sync_assetto1") a
 * montre que chaque shift H-pattern etait bien transmis instantanement et
 * correctement par le firmware (0 perte sur 39 changements de rapport),
 * mais que le jeu accusait parfois un retard de plusieurs centaines de ms a
 * plus d'1s pour refleter le changement — jamais observe avec le firmware
 * d'origine sur le meme materiel/la meme configuration AC. Le report
 * d'origine exposait en continu des axes vivants la ou le notre restait a
 * zero ; si un outil quelconque de la chaine (profil de peripherique,
 * heuristique d'entree du jeu) se base sur une activite de ces axes pour
 * distinguer un shifter "actif" d'un peripherique inerte, notre silence sur
 * ces octets pourrait expliquer un traitement different du signal bouton.
 * Pas encore confirme comme LA cause — cette modification teste
 * directement l'hypothese plutot que de continuer a la deviner.
 *
 * Aucune dependance HAL/USB : module testable hors cible (voir App/test).
 * L'integration USB (choix du timing de transmission, CDC_Transmit_FS ou
 * equivalent HID) reste dans main.c / usbd_hid_if, pas ici.
 */
#ifndef HID_REPORT_H
#define HID_REPORT_H

#include <stdint.h>

#include "mode_state_machine.h"

#define HID_REPORT_SIZE 16

/* Bits du bitmask 16 boutons (byte0 = bits 0-7, byte1 = bits 8-15),
 * mapping Phase 3 pour 1-6/11, choix retenus en Phase 5 pour 7/8. */
#define HID_BUTTON_GEAR1      0  /* byte0 bit0 */
#define HID_BUTTON_GEAR2      1  /* byte0 bit1 */
#define HID_BUTTON_GEAR3      2  /* byte0 bit2 */
#define HID_BUTTON_GEAR4      3  /* byte0 bit3 */
#define HID_BUTTON_GEAR5      4  /* byte0 bit4 */
#define HID_BUTTON_GEAR6      5  /* byte0 bit5 */
#define HID_BUTTON_SHIFT_UP   6  /* byte0 bit6 : sequentiel, rapport + */
#define HID_BUTTON_SHIFT_DOWN 7  /* byte0 bit7 : sequentiel, rapport - */
#define HID_BUTTON_REVERSE    10 /* byte1 bit2 : collerette levee + gear1 */

/* Octets 2-13 (12 des 14 octets vendor) : 6 valeurs Hall brutes (U2..U7,
 * meme ordre que hall_sample_t.v[], cf gear_position.h), chacune sur 16
 * bits little-endian — reproduit le format observe du firmware d'origine
 * (7 valeurs 16 bits LE dans les 14 octets vendor, cf hid_summary.txt ;
 * seules les 6 premieres nous concernent, la 7eme du capture original —
 * toujours petite, probablement liee a la collerette/un statut — n'a pas
 * d'equivalent connu cote firmware maison et n'est pas reproduite). */
#define HID_AXES_BYTE_OFFSET 2
#define HID_AXES_STRIDE      2 /* 2 octets par valeur (uint16 LE) */

/* Octet 14 (avant-dernier octet vendor) : etat debug, sans effet sur le
 * jeu. Deplace de l'octet 2 (qui porte desormais U2) a l'octet 14. */
#define HID_DEBUG_BYTE_INDEX          14
#define HID_DEBUG_BIT_MODE_SEQ        0 /* 0 = mode H, 1 = mode SEQ */
#define HID_DEBUG_BIT_CALIBRATION     1 /* 1 = mode calibration actif */

/* Duree de pulse (en pas d'appel a hid_report_build, pas en ms) pendant
 * laquelle un bouton "impulsion" (shift+/shift-) reste actif dans le
 * rapport apres une confirmation SEQ/HELD ponctuelle, pour laisser le temps
 * au PC/jeu de la detecter (un rapport HID qui repasse a 0 au pas suivant
 * peut etre rate si le polling du jeu est plus lent). */
#define HID_PULSE_STEPS 2

/* Chemin rapide (hid_report_apply_fast_gear) : impulsion shift+/- en ms.
 * 100ms = large devant le polling d'un jeu (>=60Hz), et assez court pour
 * enchainer des shifts rapides (~400-500ms d'ecart mini observe sur
 * test_Seq). Pause forcee FAST_PULSE_GAP_MS entre deux impulsions du meme
 * bouton pour que le jeu voie deux fronts distincts. */
#define FAST_PULSE_MS     100u
#define FAST_PULSE_GAP_MS 30u

typedef struct {
    bool active;      /* impulsion en cours */
    uint32_t end_t;   /* fin de l'impulsion (active) ou de la pause (!active) */
    int pending;      /* impulsions en attente */
} fast_pulse_t;

typedef struct {
    uint8_t report[HID_REPORT_SIZE];

    /* Compteurs de pas restants avant retombee a 0 pour chaque bouton
     * impulsionnel (gear1-6 tenus tant que la position est HELD ; shift+/-
     * et reverse sont des pulses courts independants de la duree du geste
     * physique, cf mouvement sequentiel Phase 5). */
    int gear_active; /* -1 si aucune position H-pattern tenue actuellement */
    int shift_up_pulse;
    int shift_down_pulse;

    /* Latch marche arriere : capture l'etat de la collerette au moment ou
     * gear_active PASSE a GEAR1 (transition, pas relu en continu), pour que
     * lever/relacher la collerette pendant un maintien en 1ere ne fasse pas
     * osciller le bouton transmis entre 1 et 11 a chaque pas (comportement
     * corrige suite a un test terrain, cf PLAN.md Phase 6). Vrai = bouton
     * 11 (marche arriere) pour toute la duree du maintien en 1ere. */
    int reverse_latched;

    /* Chemin rapide (hid_report_apply_fast_gear) : derniere position rapide
     * appliquee + son propre latch marche arriere (meme logique que
     * gear_active/reverse_latched mais a la cadence de la boucle rapide),
     * et impulsions shift+/- en ms (fin d'impulsion, fin de pause inter-
     * impulsions, impulsions en attente si le levier re-engage avant la fin
     * de la precedente — jamais de shift perdu ni fusionne). */
    gear_position_t fast_prev;
    int fast_reverse_latched;
    fast_pulse_t fast_up;
    fast_pulse_t fast_down;
} hid_report_state_t;

void hid_report_init(hid_report_state_t *state);

/* A appeler a chaque pas, juste apres mode_state_machine_step(). Met a jour
 * state->report en place a partir du dernier mouvement emis/mode courant.
 * collar est l'etat brut de la collerette (0/1) pour le bouton Reverse.
 * hall est l'echantillon Hall brut du meme pas (le meme que celui passe a
 * mode_state_machine_step()), copie tel quel dans les octets vendor 2-13 —
 * cf commentaire d'en-tete pour le pourquoi. */
void hid_report_build(hid_report_state_t *state, const mode_state_machine_t *msm, int collar,
                       const hall_sample_t *hall);

/* A appeler a CHAQUE iteration de la boucle rapide, apres l'eventuel
 * hid_report_build() de ce meme tour, avec la position rendue par
 * fast_gear_step() (GEAR_NONE = aucune). Hors calibration, remplace les
 * boutons issus de mode_state_machine par le chemin rapide, sans attendre
 * HOLD_TIMEOUT_MS ni le retour au neutre — cf fast_gear.h :
 *  - mode H : boutons gear1-6/marche arriere = position engagee ;
 *  - mode SEQ : une impulsion shift+/- de FAST_PULSE_MS par engagement de
 *    seq_up/seq_down (des l'arrivee du levier en butee).
 * Rafraichit aussi les octets axes avec l'echantillon Hall courant. En
 * calibration : seuls les axes sont rafraichis.
 * collar = etat collerette (anti-rebond a cadence rapide) pour le latch
 * marche arriere, capture a la transition vers GEAR_1 comme dans
 * hid_report_build. */
void hid_report_apply_fast_gear(hid_report_state_t *state, const mode_state_machine_t *msm, int collar,
                                gear_position_t fast_gear, const hall_sample_t *hall, uint32_t t_ms);

#endif /* HID_REPORT_H */
