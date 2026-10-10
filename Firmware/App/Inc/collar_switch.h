/**
 * collar_switch.h — lecture/debounce de la collerette (PB1, pull-up interne
 * active cote CubeMX, cf brochage Phase 2/config Phase 4).
 *
 * Seul module autorise a connaitre le GPIO de la collerette. Ajoute un
 * anti-rebond simple par confirmation sur COLLAR_DEBOUNCE_STEPS pas
 * consecutifs : les captures Phase 4 montraient deja des transitions
 * franches 0/1, mais mode_state_machine (blips ~200ms, maintien 2s en mode
 * calibration) est sensible a un rebond mecanique occasionnel sur la duree
 * de vie du switch — protection peu couteuse, pas presente dans
 * Diag/Gx100_DiagFirm (qui n'en avait pas besoin pour du simple logging).
 *
 * Aucune dependance HAL dans l'algorithme de debounce lui-meme (testable
 * hors cible via collar_switch_debounce_step, voir App/test) ; seule
 * collar_switch_read_raw() touche le GPIO reel.
 */
#ifndef COLLAR_SWITCH_H
#define COLLAR_SWITCH_H

#include <stdbool.h>

#define COLLAR_DEBOUNCE_STEPS 2

typedef struct {
    int stable_state;     /* 0 ou 1, dernier etat confirme */
    int candidate_state;  /* etat en cours de confirmation */
    int candidate_count;  /* nombre de pas consecutifs a candidate_state */
} collar_switch_t;

void collar_switch_init(collar_switch_t *sw);

/* Lecture brute du GPIO (PB1), 0 ou 1. Seule fonction du module qui touche
 * le HAL — implementee dans collar_switch.c, appelle HAL_GPIO_ReadPin. */
int collar_switch_read_raw(void);

/* Algorithme de debounce pur (aucune dependance HAL), appelable directement
 * en test hors cible. Retourne l'etat stable courant (0/1) apres avoir
 * integre cette nouvelle lecture brute. */
int collar_switch_debounce_step(collar_switch_t *sw, int raw_state);

/* Lit le GPIO reel puis applique le debounce en une seule fois — a appeler
 * a chaque pas dans main.c. */
int collar_switch_read(collar_switch_t *sw);

#endif /* COLLAR_SWITCH_H */
