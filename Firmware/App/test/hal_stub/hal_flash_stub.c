/*
 * hal_flash_stub.c - stub HAL_FLASH_xxx, HAL_FLASHEx_Erase et
 * flash_read_bytes pour les bancs de test hors cible qui linkent
 * calibration_store.c (et donc calibration_flash.c, compile avec
 * -DCALIBRATION_FLASH_READ_OVERRIDE pour ces cibles) mais ne testent pas
 * la persistance flash elle-meme (test_replay.c, test_hid_report.c,
 * test_hid_replay.c, test_fast_gear.c, test_debug_trace.c).
 *
 * Les fonctions HAL_FLASH_xxx renvoient systematiquement HAL_ERROR :
 * save() echoue silencieusement, sans consequence pour ces tests qui ne
 * relisent jamais apres un save(). flash_read_bytes() ne touche jamais de
 * memoire reelle (0x0800FC00 n'existe pas sur l'hote x86/x64) : elle
 * remplit *out avec un pattern qui ne matchera jamais
 * CALIBRATION_FLASH_MAGIC, donc calibration_flash_load() echoue toujours
 * proprement (comportement "flash vierge", calibration_store retombe sur
 * les defauts, exactement le comportement deja teste avant l'introduction
 * de calibration_flash).
 *
 * Voir test_calibration_flash.c pour les tests qui simulent une vraie
 * flash en RAM et verifient reellement save()+load().
 */
#include <string.h>

#include "main.h"

void flash_read_bytes(uintptr_t addr, void *out, size_t len)
{
    (void)addr;
    memset(out, 0x00, len); /* jamais egal a CALIBRATION_FLASH_MAGIC */
}

HAL_StatusTypeDef HAL_FLASH_Unlock(void)
{
    return HAL_ERROR;
}

HAL_StatusTypeDef HAL_FLASH_Lock(void)
{
    return HAL_OK;
}

HAL_StatusTypeDef HAL_FLASH_Program(uint32_t TypeProgram, uint32_t Address, uint64_t Data)
{
    (void)TypeProgram;
    (void)Address;
    (void)Data;
    return HAL_ERROR;
}

HAL_StatusTypeDef HAL_FLASHEx_Erase(FLASH_EraseInitTypeDef *pEraseInit, uint32_t *PageError)
{
    (void)pEraseInit;
    if (PageError != 0) {
        *PageError = 0;
    }
    return HAL_ERROR;
}
