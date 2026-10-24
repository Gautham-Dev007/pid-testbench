#ifndef METRICS_H
#define METRICS_H

#include <stdbool.h>
#include <stdint.h>

#include "controller.h"

/* Step-response figures for the current run. A step starts on START, RESET,
 * a setpoint change, a mode change, or the hand-over after autotune.
 *
 *   rise       10 % -> 90 % of the step
 *   to SP      first time the setpoint is reached
 *   overshoot  peak beyond SP, in degC and % of the step
 *   settle     time of the last entry into the band (2 % of the step)
 *   IAE        integral of |error| dt, handy for comparing modes */

void metrics_start(float setpoint, float temp, uint32_t now_ms);
void metrics_update(float setpoint, float temp, float dt, uint32_t now_ms);
void metrics_print(const char *mode, Gains g, uint32_t now_ms);

/* True once per step, when the response has stayed in band long enough. */
bool metrics_take_settled_event(void);

#endif
