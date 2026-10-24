#include "autotune.h"

#include <math.h>
#include <string.h>

#if ZN_RULE == 0
#define ZN_KP 0.60f
#define ZN_TI 0.50f
#define ZN_TD 0.125f
#elif ZN_RULE == 1
#define ZN_KP 0.20f
#define ZN_TI 0.50f
#define ZN_TD (1.0f / 3.0f)
#elif ZN_RULE == 2
#define ZN_KP 0.45f
#define ZN_TI (1.0f / 1.2f)
#define ZN_TD 0.0f
#else
#error "ZN_RULE must be 0, 1 or 2"
#endif

#define PI_F 3.14159265f

static struct {
  bool     running;
  bool     relay_on;
  uint8_t  switches;
  uint32_t start_ms;
  uint32_t switch_ms[RELAY_SWITCHES + 1];
  float    extreme[RELAY_SWITCHES + 1];  /* extreme of the phase ended by switch k */
  float    phase_max, phase_min;

  TuneResult  result;
  const char *fail_reason;
} t;

void tune_start(uint32_t now_ms)
{
  memset(&t, 0, sizeof t);
  t.running     = true;
  t.relay_on    = true;
  t.start_ms    = now_ms;
  t.phase_max   = -1000.0f;
  t.phase_min   = 1000.0f;
  t.fail_reason = "";
}

void tune_abort(void) { t.running = false; }

static TuneStatus finish(void)
{
  t.running = false;

  /* Odd switches end ON phases (troughs), even ones end OFF phases (peaks). */
  float peak   = (t.extreme[4] + t.extreme[6]) * 0.5f;
  float trough = (t.extreme[3] + t.extreme[5]) * 0.5f;
  float a      = (peak - trough) * 0.5f;

  float tu = ((float)(t.switch_ms[5] - t.switch_ms[3]) +
              (float)(t.switch_ms[6] - t.switch_ms[4])) * 0.5f / 1000.0f;

  float on_ms  = (float)(t.switch_ms[3] - t.switch_ms[2]) + (float)(t.switch_ms[5] - t.switch_ms[4]);
  float off_ms = (float)(t.switch_ms[4] - t.switch_ms[3]) + (float)(t.switch_ms[6] - t.switch_ms[5]);

  t.result.amplitude = a;
  t.result.tu        = tu;
  t.result.hold_pct  = (on_ms + off_ms > 0.0f) ? 100.0f * on_ms / (on_ms + off_ms) : 50.0f;

  /* The amplitude has to clear the relay hysteresis by a margin, or the
   * describing-function estimate below means nothing. */
  if (!(a > 2.0f * RELAY_HYST_C)) {
    t.fail_reason = "oscillation too small";
    return TUNE_FAILED;
  }
  if (!(tu > 2.0f)) {
    t.fail_reason = "period too short";
    return TUNE_FAILED;
  }

  float ku = 4.0f * RELAY_AMPLITUDE / (PI_F * sqrtf(a * a - RELAY_HYST_C * RELAY_HYST_C));
  if (!isfinite(ku) || ku <= 0.0f) {
    t.fail_reason = "bad Ku";
    return TUNE_FAILED;
  }

  float kp = ZN_KP * ku;
  t.result.ku    = ku;
  t.result.gains = (Gains){kp, kp / (ZN_TI * tu), kp * ZN_TD * tu};
  return TUNE_DONE;
}

TuneStatus tune_update(float temp, float setpoint, uint32_t now_ms)
{
  if (!t.running) return TUNE_FAILED;

  if (tune_elapsed_s(now_ms) > AUTOTUNE_TIMEOUT_S) {
    t.running     = false;
    t.fail_reason = t.switches == 0 ? "timeout, setpoint never reached" : "timeout";
    return TUNE_FAILED;
  }

  t.phase_max = fmaxf(t.phase_max, temp);
  t.phase_min = fminf(t.phase_min, temp);

  bool switched = false;
  if (t.relay_on && temp > setpoint + RELAY_HYST_C) {
    t.relay_on = false;
    switched   = true;
    t.extreme[t.switches + 1] = t.phase_min;  /* an ON phase ends at its trough */
  } else if (!t.relay_on && temp < setpoint - RELAY_HYST_C) {
    t.relay_on = true;
    switched   = true;
    t.extreme[t.switches + 1] = t.phase_max;  /* an OFF phase ends at its peak */
  }

  if (switched) {
    t.switches++;
    t.switch_ms[t.switches] = now_ms - t.start_ms;
    t.phase_max = t.phase_min = temp;
    if (t.switches >= RELAY_SWITCHES) return finish();
  }
  return TUNE_RUNNING;
}

bool     tune_running(void)              { return t.running; }
float    tune_heater_pct(void)           { return t.relay_on ? 100.0f : 0.0f; }
uint8_t  tune_switches(void)             { return t.switches; }
uint32_t tune_elapsed_s(uint32_t now_ms) { return (now_ms - t.start_ms) / 1000u; }

const TuneResult *tune_result(void)      { return &t.result; }
const char       *tune_fail_reason(void) { return t.fail_reason; }
