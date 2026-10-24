#include "main.h"
#include "stm32g0xx_it.h"

/* USER CODE BEGIN Includes */
#include "tim.h"
#include "usart.h"
/* USER CODE END Includes */

void NMI_Handler(void)
{
  while (1)
  {
  }
}

/* On a hard fault the heater pin is forced high (SSR off) before hanging;
 * the watchdog then resets the board. */
void HardFault_Handler(void)
{
  /* USER CODE BEGIN HardFault_IRQn 0 */
  TIM1->BDTR &= ~TIM_BDTR_MOE;
  GPIOB->BSRR = GPIO_PIN_3;
  GPIOB->MODER = (GPIOB->MODER & ~(3u << (3 * 2))) | (1u << (3 * 2));
  /* USER CODE END HardFault_IRQn 0 */
  while (1)
  {
  }
}

void SVC_Handler(void)
{
}

void PendSV_Handler(void)
{
}

void SysTick_Handler(void)
{
  HAL_IncTick();
}

void TIM6_IRQHandler(void)
{
  HAL_TIM_IRQHandler(&htim6);
}

void USART2_IRQHandler(void)
{
  HAL_UART_IRQHandler(&huart2);
}
