#include "controller.h"

#include <math.h>

#define MAX_DT_S 0.5f /* longer gaps (fault, stall) restart the history */

typedef struct {
  float above, kp, ki;
} Zone;

static const Zone zones[ADAPTIVE_ZONE_COUNT] = ADAPTIVE_ZONES_INIT;

static struct {
  ControlMode mode;
  Gains       manual, tuned, active;
  bool        have_tuned;
  int8_t      zone;

  float i_term;   /* % */
  float slope;    /* filtered dT/dt for the D term */
  float prev_temp;
  bool  have_prev;
} c;

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

void ctrl_init(void)
{
  c.mode       = START_MODE;
  c.manual     = (Gains){MANUAL_KP, MANUAL_KI, MANUAL_KD};
  c.tuned      = c.manual;
  c.active     = c.manual;
  c.have_tuned = false;
  ctrl_reset();
}

void ctrl_set_mode(ControlMode mode)
{
  if (mode > MODE_ADAPTIVE) return;
  c.mode = mode;
  c.zone = -1;
  /* The integral stays: it holds the power needed at this temperature, and
   * since it's stored in % it means the same thing under the new gains. */
}

ControlMode ctrl_mode(void) { return c.mode; }

const char *ctrl_mode_name(void)
{
  switch (c.mode) {
    case MODE_MANUAL:   return "MANUAL";
    case MODE_ZN:       return "ZN";
    case MODE_ADAPTIVE: return "ADAPT";
  }
  return "?";
}

void  ctrl_set_manual_gains(Gains g) { c.manual = g; }
void  ctrl_set_tuned_gains(Gains g)  { c.tuned = g; c.have_tuned = true; }
void  ctrl_clear_tuned_gains(void)   { c.have_tuned = false; }
bool  ctrl_has_tuned_gains(void)     { return c.have_tuned; }
Gains ctrl_tuned_gains(void)         { return c.tuned; }
Gains ctrl_active_gains(void)        { return c.active; }
int8_t ctrl_zone(void)               { return c.zone; }

void ctrl_reset(void)
{
  c.i_term    = 0.0f;
  c.slope     = 0.0f;
  c.have_prev = false;
  c.zone      = -1;
}

void ctrl_preload_integral(float pct) { c.i_term = clampf(pct, 0.0f, 100.0f); }

static void select_gains(float abs_error)
{
  switch (c.mode) {
    case MODE_MANUAL:
      c.active = c.manual;
      break;

    case MODE_ZN:
      c.active = c.have_tuned ? c.tuned : c.manual;
      break;

    case MODE_ADAPTIVE: {
      /* Each threshold moves by the hysteresis depending on which side of it
       * we are, so noise on a boundary doesn't flip Kp every sample. */
      int8_t z = ADAPTIVE_ZONE_COUNT - 1;
      for (int8_t i = 0; i < ADAPTIVE_ZONE_COUNT - 1; i++) {
        float threshold = zones[i].above;
        if (c.zone >= 0) threshold += (c.zone <= i) ? -ADAPTIVE_ZONE_HYST : ADAPTIVE_ZONE_HYST;
        if (abs_error > threshold) {
          z = i;
          break;
        }
      }
      c.zone   = z;
      c.active = (Gains){zones[z].kp, zones[z].ki, ADAPTIVE_KD};
      break;
    }
  }
}

float ctrl_update(float setpoint, float temp, float dt)
{
  bool dt_ok = dt > 0.0f && dt <= MAX_DT_S;
  if (!dt_ok) c.have_prev = false;

  float error = setpoint - temp;
  select_gains(fabsf(error));
  const Gains *g = &c.active;

  float p = g->kp * error;

  float d = 0.0f;
  if (c.have_prev) {
    float rate = (temp - c.prev_temp) / dt;
    c.slope += dt / (PID_D_FILTER_TAU_S + dt) * (rate - c.slope);
    d = clampf(-g->kd * c.slope, -PID_D_LIMIT_PCT, PID_D_LIMIT_PCT);
  } else {
    c.slope = 0.0f;
  }
  c.prev_temp = temp;
  c.have_prev = true;

  float base_band = (c.mode == MODE_ADAPTIVE) ? ADAPTIVE_INTEGRAL_BAND_C : PID_INTEGRAL_BAND_C;
  float band      = fmaxf(base_band, PID_PBAND_PCT / fmaxf(g->kp, 0.01f));

  bool saturated_high = (p + c.i_term + d >= 100.0f) && error > 0.0f;
  bool in_band        = error < 0.0f || error <= band;

  if (dt_ok && in_band && !saturated_high) {
    float step = g->ki * error * dt;
    if (error < 0.0f) step *= PID_UNWIND;
    c.i_term += step;
  }
  c.i_term = clampf(c.i_term, 0.0f, 100.0f);

  return clampf(p + c.i_term + d, 0.0f, 100.0f);
}
