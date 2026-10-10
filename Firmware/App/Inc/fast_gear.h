/**
 * fast_gear.h — detection rapide de la position du levier (gear1..6 en mode
 * H, seq_up/seq_down en mode SEQ), en parallele de mode_state_machine.
 *
 * Pourquoi : mode_state_machine tourne a 100ms/echantillon et ne confirme
 * une position qu'apres CONFIRM_SAMPLES (2) + HOLD_TIMEOUT_MS (250ms), ou
 * au retour au neutre pour une impulsion sequentielle. Mesure sur captures
 * reelles : en H (sync_assetto2/3), le levier est stabilise dans la grille
 * 300 a 500ms AVANT l'envoi du bouton ; en SEQ (test_Seq), le levier est
 * sur le point seq_up/seq_down des le 1er echantillon hors neutre, mais le
 * shift+/- ne part qu'au RELACHEMENT du levier (~200-300ms) ou apres
 * ~400ms si le levier est maintenu. Resultat en jeu : rapport qui arrive
 * apres la fin du geste, rapports rates.
 *
 * Le mode courant est deja connu (geste de bascule explicite), on ne
 * cherche donc plus a distinguer SEQ/HELD par la duree. Un echantillon
 * engage la position p si :
 *  - p est la plus proche des positions calibrees (neutre compris ; en
 *    SEQ, les points H sont exclus car hors d'atteinte levier verrouille,
 *    et un seq_down appuye se rapproche de gear3) — evite qu'un seq_down
 *    (a 0.30 de gear3) n'engage gear3 en mode H pendant le geste de
 *    bascule, et qu'un neutre bruite n'engage seq_up (a 0.46 seulement
 *    du neutre) ;
 *  - p appartient au mode courant ;
 *  - distance a p < FAST_GEAR_ENGAGE_DIST et distance au neutre >
 *    FAST_GEAR_MIN_NEUTRAL_DIST (marge anti-bruit au repos, utile surtout
 *    pour seq_up, course courte) ;
 *  - p devance la 2eme position la plus proche d'au moins
 *    FAST_GEAR_MARGIN : gear3/seq_down ne sont qu'a 0.30 l'un de l'autre,
 *    un seq_down du geste de bascule H->SEQ tombe parfois a mi-chemin
 *    (vu sur test_H_pattern9/test_Ben : ecart 0.00-0.05), alors qu'une
 *    vraie 3eme devance seq_down d'au moins ~0.11 sur toutes les captures ;
 *  - le tout stable FAST_GEAR_ENGAGE_MS.
 * Apres un changement de mode (geste de bascule), rien n'est engage tant
 * que le levier n'est pas repasse par le neutre : le geste vers H se fait
 * en maintenant gear6, qui ne doit pas partir en jeu a la fin du geste
 * (meme comportement que le chemin lent).
 * La position est relachee quand le levier s'eloigne au-dela de
 * FAST_GEAR_RELEASE_DIST ou revient au neutre (GEAR_NEUTRAL_DIST_THRESHOLD)
 * pendant FAST_GEAR_RELEASE_MS (hysteresis : pas de clignotement en bord
 * de grille, pas de double shift).
 *
 * Seuils (defauts calibres, cf PLAN.md) : dans la grille p90 ~0.15 ;
 * ecart minimal entre deux points H ~0.83 ; seq_up/seq_down au repos
 * ~0.05-0.20 du point calibre et 0.45-1.0 du neutre.
 *
 * mode_state_machine reste inchange et continue de piloter gestes de
 * bascule et calibration a sa cadence validee.
 *
 * Aucune dependance HAL/USB : module testable hors cible (voir App/test).
 */
#ifndef FAST_GEAR_H
#define FAST_GEAR_H

#include <stdint.h>

#include "gear_position.h"

#define FAST_GEAR_ENGAGE_DIST      0.35f
#define FAST_GEAR_MIN_NEUTRAL_DIST 0.35f
#define FAST_GEAR_MARGIN           0.08f
#define FAST_GEAR_RELEASE_DIST     0.60f
#define FAST_GEAR_ENGAGE_MS        6u  /* anti-pic : meme candidat stable ce temps avant engagement */
#define FAST_GEAR_RELEASE_MS       15u /* hors grille ce temps avant relachement */

typedef struct {
    gear_mode_t mode;          /* mode de la derniere evaluation (reset si change) */
    bool wait_neutral;         /* apres changement de mode : attendre le neutre */
    gear_position_t engaged;   /* GEAR_NONE si aucune position engagee */
    gear_position_t candidate; /* GEAR_NONE si aucun candidat en cours */
    uint32_t candidate_t;
    bool leaving;
    uint32_t leave_t;

    /* Points de reference normalises, recalcules seulement quand
     * calibration->positions[] change (neutral/scale en sont derives) :
     * evite 9 normalisations en flottant logiciel a chaque pas rapide. */
    bool ref_valid;
    hall_sample_t ref_key[GEAR_POSITION_COUNT];
    hall_norm_t ref_norm[GEAR_POSITION_COUNT];
} fast_gear_t;

void fast_gear_init(fast_gear_t *fg);

/* A appeler a chaque echantillon Hall (cadence rapide), avec le mode
 * courant (GEAR_MODE_H ou GEAR_MODE_SEQ). Retourne la position engagee
 * (GEAR_1..6 en H, GEAR_SEQ_UP/DOWN en SEQ) ou GEAR_NONE. */
gear_position_t fast_gear_step(fast_gear_t *fg, uint32_t t_ms, const hall_sample_t *hall,
                               const gear_calibration_t *calibration, gear_mode_t mode);

#endif /* FAST_GEAR_H */
