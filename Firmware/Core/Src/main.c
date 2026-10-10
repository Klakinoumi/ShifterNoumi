/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "adc.h"
#include "dma.h"
#include "usb_device.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "usbd_composite.h"

#include "hall_sensors.h"
#include "collar_switch.h"
#include "gear_position.h"
#include "calibration_store.h"
#include "mode_state_machine.h"
#include "hid_report.h"
#include "fast_gear.h"
#include "debug_trace.h"
#include "status_led.h"
#include "boot_link.h"
#include "mode_flash.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* Cadence du pas lent (mode_state_machine, gestes, calibration, trace
 * CDC, LED) : conservee a 100ms/pas comme le
 * firmware de diagnostic (Diag/Gx100_DiagFirm), car toutes les constantes
 * de mode_state_machine.h (CONFIRM_SAMPLES, HOLD_TIMEOUT_MS, HELD_WINDOW,
 * timings du geste de bascule/calibration) ont ete calibrees empiriquement
 * sur des captures reelles a cette cadence — la changer sans revalider sur
 * de nouvelles captures risquerait de modifier le comportement deja
 * valide (cf PLAN.md Phase 5). */
#define MAIN_LOOP_PERIOD_MS 100U
/* Boucle rapide : lecture Hall + fast_gear + envoi HID a chaque tick.
 * mode_state_machine & co restent cadences a MAIN_LOOP_PERIOD_MS (cf
 * ci-dessus) ; seul le chemin rapide H + SEQ (fast_gear.h) profite de
 * cette cadence, pour supprimer les 300-500ms de latence mesures entre
 * l'arrivee du levier en grille et l'envoi du bouton. */
#define FAST_LOOP_PERIOD_MS 1U
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
static gear_calibration_t g_calibration;
static mode_state_machine_t g_msm;
static collar_switch_t g_collar;
/* Anti-rebond separe a cadence rapide (COLLAR_DEBOUNCE_STEPS x 1ms) pour le
 * latch marche arriere du chemin rapide : g_collar reste a la cadence
 * lente, dont dependent les timings valides des gestes/calibration. */
static collar_switch_t g_collar_fast;
static fast_gear_t g_fast_gear;
static hid_report_state_t g_hid_report;
static status_led_state_t g_status_led;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */
  /* Table des vecteurs la ou ce binaire a ete lie : 0x08000000 seul, ou
   * 0x08001000 derriere le bootloader USB (cf gx100_boot.h). A faire avant
   * HAL_Init, qui active l'interruption SysTick. */
  extern uint32_t g_pfnVectors[];
  SCB->VTOR = (uint32_t)g_pfnVectors;
  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */
  /* Force l'hote a re-enumerer apres un reset sans debranchement (mise a
   * jour, sortie du bootloader), cf boot_link.h. */
  boot_link_usb_reenumerate();
  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_ADC1_Init();
  MX_USB_DEVICE_Init();
  /* USER CODE BEGIN 2 */
  hall_sensors_init();
  collar_switch_init(&g_collar);
  collar_switch_init(&g_collar_fast);
  fast_gear_init(&g_fast_gear);

  /* calibration_store_load() lit la derniere calibration valide en flash
   * (calibration_flash.c) et retombe sur les defauts mesures Phase 4/5 si
   * la flash est vierge ou corrompue — dans les deux cas, neutral/scale
   * sont deja recalcules a la sortie de cet appel (cf calibration_store.c). */
  calibration_store_load(&g_calibration);
  mode_state_machine_init(&g_msm, &g_calibration);
  /* Reprend le mode du branchement precedent (H par defaut si aucun). */
  gear_mode_t saved_mode = mode_flash_load();
  if (saved_mode != GEAR_MODE_NONE) {
    g_msm.mode_courant = saved_mode;
  }
  hid_report_init(&g_hid_report);

  /* debug_trace : sortie CSV sur le port CDC (2eme interface du composite
   * HID+CDC, cf usbd_composite.c), en parallele du HID gamepad qui reste
   * inchange. Permet de diagnostiquer boutons/bascule de mode avec un
   * script Python (pyserial) sans logiciel PC proprietaire ni sacrifier le
   * device HID (cf PLAN.md Phase 6). */
  debug_trace_init();

  /* status_led : retour visuel sur la LED integree PC13 pendant le mode
   * calibration (respiration + point attendu en impulsions, flash de
   * validation par point, cf status_led.h) — pas de logiciel PC
   * necessaire pour suivre une session de calibration sur le terrain. */
  status_led_init(&g_status_led);
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  uint32_t t_last_slow = HAL_GetTick() - MAIN_LOOP_PERIOD_MS; /* 1er pas lent immediat */
  while (1)
  {
    uint32_t t_loop = HAL_GetTick();
    hall_sample_t hall;
    hall_sensors_read(&hall);
    uint32_t t_ms = HAL_GetTick();

    /* --- Pas lent (cadence validee 100ms) : inchange par rapport a la
     * boucle d'origine. */
    bool slow_step = (t_ms - t_last_slow) >= MAIN_LOOP_PERIOD_MS;
    int collar = 0;
    if (slow_step) {
      t_last_slow = t_ms;
      collar = collar_switch_read(&g_collar);
      mode_state_machine_step(&g_msm, t_ms, &hall, collar);
      if (g_msm.last_switch_applied) {
        mode_flash_save(g_msm.mode_courant);
      }
      hid_report_build(&g_hid_report, &g_msm, collar, &hall);
      status_led_step(&g_status_led, t_ms, &g_msm);
    }

    /* --- Pas rapide : position H / impulsion SEQ sans attendre la confirmation
     * lente (cf fast_gear.h). g_msm.calibration = calibration courante,
     * y compris apres une mise a jour en mode calibration. */
    int collar_fast = collar_switch_read(&g_collar_fast);
    gear_position_t fast_gear = fast_gear_step(&g_fast_gear, t_ms, &hall, &g_msm.calibration, g_msm.mode_courant);
    hid_report_apply_fast_gear(&g_hid_report, &g_msm, collar_fast, fast_gear, &hall, t_ms);

    /* Trace CDC apres le pas rapide : la ligne montre le rapport HID
     * reellement envoye ce tour (cf debug_trace.h). */
    if (slow_step) {
      debug_trace_step(t_ms, &hall, collar, &g_msm, g_hid_report.report, fast_gear);
    }
    debug_trace_calibration_poll(t_ms, &g_msm.calibration);
    status_led_output(&g_status_led, t_ms);

    /* Non bloquant : ignore si le transfert precedent n'est pas termine, le
     * rapport courant repart au tour suivant. */
    USBD_COMPOSITE_HID_SendReport(&hUsbDeviceFS, g_hid_report.report, HID_REPORT_SIZE);

    /* Commande "passage en bootloader" recue sur le port CDC : reset. */
    boot_link_poll();

    /* Attend le tick suivant le debut de ce tour (pas HAL_Delay, qui ajoute
     * un tick de marge et donnait une periode de ~2ms) : les tours suivants
     * demarrent sur un front de SysTick, periode = FAST_LOOP_PERIOD_MS. */
    while ((HAL_GetTick() - t_loop) < FAST_LOOP_PERIOD_MS)
    {
    }
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
  PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_ADC|RCC_PERIPHCLK_USB;
  PeriphClkInit.AdcClockSelection = RCC_ADCPCLK2_DIV6;
  PeriphClkInit.UsbClockSelection = RCC_USBCLKSOURCE_PLL_DIV1_5;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
