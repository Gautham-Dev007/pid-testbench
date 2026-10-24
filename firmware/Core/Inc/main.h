/* USER CODE BEGIN Header */
/*
 * PID temperature testbench, NUCLEO-G070RB.
 */
/* USER CODE END Header */

#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32g0xx_hal.h"

/* USER CODE BEGIN Includes */
#include "app_config.h"
/* USER CODE END Includes */

void Error_Handler(void);

/* Private defines -----------------------------------------------------------*/
#define BUZZER_Pin GPIO_PIN_10
#define BUZZER_GPIO_Port GPIOA
#define FAN2_DIR_Pin GPIO_PIN_9
#define FAN2_DIR_GPIO_Port GPIOA
#define RTD_CS_Pin GPIO_PIN_0
#define RTD_CS_GPIO_Port GPIOB
#define FAN1_DIR_Pin GPIO_PIN_14
#define FAN1_DIR_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
