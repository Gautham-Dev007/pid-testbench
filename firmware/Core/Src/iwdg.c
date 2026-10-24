#include "iwdg.h"

IWDG_HandleTypeDef hiwdg;

/* Independent watchdog: 32 kHz LSI / 32 = 1 kHz, reload 2000 -> about 2 s.
 * If the main loop ever hangs (a stuck I2C bus, say) the chip resets and
 * comes back up with the heater off. */
void MX_IWDG_Init(void)
{
  hiwdg.Instance = IWDG;
  hiwdg.Init.Prescaler = IWDG_PRESCALER_32;
  hiwdg.Init.Window = 4095;
  hiwdg.Init.Reload = 2000;
  if (HAL_IWDG_Init(&hiwdg) != HAL_OK)
  {
    Error_Handler();
  }
}
