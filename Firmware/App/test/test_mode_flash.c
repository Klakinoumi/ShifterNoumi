/**
 * test_mode_flash.c — teste mode_flash.c hors cible, page flash simulee en
 * RAM avec la vraie semantique (program = AND, erase = 0xFF), comme
 * test_calibration_flash.c.
 *
 * Build : gcc -std=c99 -Wall -Wextra -DCALIBRATION_FLASH_READ_OVERRIDE \
 *   -I ../Inc -I hal_stub test_mode_flash.c ../Src/mode_flash.c -o test_mode_flash
 */
#include <stdio.h>
#include <string.h>

#include "main.h" /* hal_stub/main.h */
#include "mode_flash.h"

static uint8_t g_page[MODE_FLASH_PAGE_SIZE];
static int g_programs;
static int g_erases;

HAL_StatusTypeDef HAL_FLASH_Unlock(void) { return HAL_OK; }
HAL_StatusTypeDef HAL_FLASH_Lock(void) { return HAL_OK; }

HAL_StatusTypeDef HAL_FLASH_Program(uint32_t TypeProgram, uint32_t Address, uint64_t Data)
{
    (void)TypeProgram;
    uint32_t offset = Address - MODE_FLASH_PAGE_ADDR;
    if (offset + 2 > sizeof(g_page)) {
        return HAL_ERROR;
    }
    uint16_t existing, hw = (uint16_t)Data;
    memcpy(&existing, &g_page[offset], 2);
    existing &= hw;
    memcpy(&g_page[offset], &existing, 2);
    g_programs++;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_FLASHEx_Erase(FLASH_EraseInitTypeDef *pEraseInit, uint32_t *PageError)
{
    if (pEraseInit->PageAddress != MODE_FLASH_PAGE_ADDR || pEraseInit->NbPages != 1) {
        return HAL_ERROR; /* ne doit jamais toucher une autre page */
    }
    memset(g_page, 0xFF, sizeof(g_page));
    *PageError = 0xFFFFFFFFUL;
    g_erases++;
    return HAL_OK;
}

void flash_read_bytes(uintptr_t addr, void *out, size_t len)
{
    memcpy(out, &g_page[addr - MODE_FLASH_PAGE_ADDR], len);
}

static int failures = 0;
#define CHECK(cond, msg)                                                                                             \
    do {                                                                                                             \
        if (!(cond)) {                                                                                               \
            printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);                                                   \
            failures++;                                                                                              \
        }                                                                                                            \
    } while (0)

int main(void)
{
    memset(g_page, 0xFF, sizeof(g_page));
    CHECK(mode_flash_load() == GEAR_MODE_NONE, "page vierge : aucun mode");

    CHECK(mode_flash_save(GEAR_MODE_SEQ), "sauvegarde SEQ");
    CHECK(mode_flash_load() == GEAR_MODE_SEQ, "relit SEQ");
    int programs = g_programs;
    CHECK(mode_flash_save(GEAR_MODE_SEQ), "sauvegarde SEQ identique");
    CHECK(g_programs == programs, "mode identique : aucune ecriture");
    CHECK(mode_flash_save(GEAR_MODE_H), "sauvegarde H");
    CHECK(mode_flash_load() == GEAR_MODE_H, "relit H");
    CHECK(!mode_flash_save(GEAR_MODE_NONE), "GEAR_MODE_NONE refuse");

    /* Coupure pendant une programmation : enregistrement incomplet apres
     * le dernier valide. Le chargement l'ignore, la sauvegarde suivante
     * ecrit apres lui. */
    g_page[4] = 0x00;
    g_page[5] = 0x53;
    CHECK(mode_flash_load() == GEAR_MODE_H, "enregistrement casse ignore");
    CHECK(mode_flash_save(GEAR_MODE_SEQ), "sauvegarde apres coupure");
    CHECK(mode_flash_load() == GEAR_MODE_SEQ, "relit SEQ apres coupure");

    /* Remplissage complet : effacement puis reprise au debut. */
    for (int i = 0; i < (int)MODE_FLASH_RECORD_COUNT + 5; i++) {
        CHECK(mode_flash_save(i % 2 ? GEAR_MODE_SEQ : GEAR_MODE_H), "sauvegarde en boucle");
    }
    CHECK(g_erases == 1, "une seule page effacee apres 512 enregistrements");
    CHECK(mode_flash_load() == GEAR_MODE_H, "dernier mode apres rotation");

    /* Page remplie de contenu etranger (firmware precedent) : aucun mode,
     * et la sauvegarde efface puis ecrit. */
    for (size_t i = 0; i < sizeof(g_page); i++) {
        g_page[i] = (uint8_t)(i * 37 + 1);
    }
    CHECK(mode_flash_load() == GEAR_MODE_NONE, "contenu etranger : aucun mode");
    CHECK(mode_flash_save(GEAR_MODE_SEQ), "sauvegarde sur contenu etranger");
    CHECK(mode_flash_load() == GEAR_MODE_SEQ, "relit SEQ apres effacement");

    if (failures == 0) {
        printf("OK: tous les tests mode_flash ont reussi.\n");
    } else {
        printf("%d test(s) en echec.\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
