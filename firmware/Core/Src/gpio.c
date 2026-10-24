#include "gpio.h"

/* Plain GPIO: RTD chip select, the two "B" bridge inputs held low, and the
 * buzzer pin that TIM6 toggles. */
void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  HAL_GPIO_WritePin(GPIOA, BUZZER_Pin | FAN2_DIR_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(FAN1_DIR_GPIO_Port, FAN1_DIR_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(RTD_CS_GPIO_Port, RTD_CS_Pin, GPIO_PIN_SET);

  GPIO_InitStruct.Pin = BUZZER_Pin | FAN2_DIR_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  GPIO_InitStruct.Pin = RTD_CS_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(RTD_CS_GPIO_Port, &GPIO_InitStruct);

  GPIO_InitStruct.Pin = FAN1_DIR_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(FAN1_DIR_GPIO_Port, &GPIO_InitStruct);
}
