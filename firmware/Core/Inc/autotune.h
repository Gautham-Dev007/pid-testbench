#ifndef AUTOTUNE_H
#define AUTOTUNE_H

#include <stdbool.h>
#include <stdint.h>

#include "controller.h"

/* Hysteresis relay test (Astrom-Hagglund). The heater toggles 0/100 % around
 * the setpoint. The first two switches contain the initial heat-up and are
 * ignored; switches 3..6 give the peaks, troughs and period. */

typedef enum { TUNE_RUNNING, TUNE_DONE, TUNE_FAILED } TuneStatus;

typedef struct {
  float ku;         /* ultimate gain, % per degC */
  float tu;         /* ultimate period, s */
  float amplitude;  /* half peak-to-peak of the oscillation, degC */
  float hold_pct;   /* average relay duty, i.e. power needed to hold SP */
  Gains gains;
} TuneResult;

void       tune_start(uint32_t now_ms);
void       tune_abort(void);
TuneStatus tune_update(float temp, float setpoint, uint32_t now_ms);

bool     tune_running(void);
float    tune_heater_pct(void);
uint8_t  tune_switches(void);
uint32_t tune_elapsed_s(uint32_t now_ms);

const TuneResult *tune_result(void);
const char       *tune_fail_reason(void);

#endif
