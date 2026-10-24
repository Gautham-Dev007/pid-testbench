#ifndef CONTROLLER_H
#define CONTROLLER_H

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"

/* The three algorithms under comparison share one PID core:
 *  - derivative on measurement (no kick on setpoint steps), low-pass filtered
 *  - the integral is stored as its contribution in %, so changing Ki never
 *    makes the output jump and gain scheduling needs no rescaling
 *  - conditional integration plus a fast unwind above SP, since the plant
 *    can only heat
 * Fans are never fed into the controller, which keeps the comparison fair. */

typedef struct {
  float kp, ki, kd;
} Gains;

void        ctrl_init(void);
void        ctrl_set_mode(ControlMode mode);
ControlMode ctrl_mode(void);
const char *ctrl_mode_name(void);

void  ctrl_set_manual_gains(Gains g);
void  ctrl_set_tuned_gains(Gains g);
void  ctrl_clear_tuned_gains(void);
bool  ctrl_has_tuned_gains(void);
Gains ctrl_tuned_gains(void);

/* Gains used on the last update; in ADAPTIVE these follow the zone. */
Gains  ctrl_active_gains(void);
int8_t ctrl_zone(void);

void ctrl_reset(void);
void ctrl_preload_integral(float pct);

/* dt in seconds since the previous call. Returns heater output 0..100 %. */
float ctrl_update(float setpoint, float temp, float dt);

#endif
