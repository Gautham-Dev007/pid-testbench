#include "metrics.h"

#include <math.h>

#include "console.h"
#include "fmt.h"

static struct {
  bool     active;
  float    start_temp, setpoint, step, band;
  uint32_t t0;

  float rise_start, rise_end, reached_at;
  float peak, peak_at;
  float settle_at;
  bool  in_band, settled, settled_event;
  float iae;
} m;

void metrics_start(float setpoint, float temp, uint32_t now_ms)
{
  m.active     = true;
  m.start_temp = temp;
  m.setpoint   = setpoint;
  m.step       = setpoint - temp;
  m.band       = fmaxf(SETTLE_BAND_FRAC * fabsf(m.step), SETTLE_BAND_MIN_C);
  m.t0         = now_ms;

  m.rise_start = m.rise_end = m.reached_at = -1.0f;
  m.peak       = temp;
  m.peak_at    = -1.0f;
  m.settle_at  = -1.0f;
  m.in_band = m.settled = m.settled_event = false;
  m.iae        = 0.0f;
}

void metrics_update(float setpoint, float temp, float dt, uint32_t now_ms)
{
  if (!m.active) return;
  if (setpoint != m.setpoint) metrics_start(setpoint, temp, now_ms);

  float t     = (float)(now_ms - m.t0) / 1000.0f;
  float error = setpoint - temp;
  if (dt > 0.0f && dt < 1.0f) m.iae += fabsf(error) * dt;

  if (fabsf(m.step) >= MIN_STEP_C) {
    float progress = (temp - m.start_temp) / m.step;
    if (m.rise_start < 0 && progress >= 0.1f) m.rise_start = t;
    if (m.rise_end < 0 && progress >= 0.9f)   m.rise_end = t;
    if (m.reached_at < 0 && progress >= 1.0f) m.reached_at = t;

    bool further = m.step > 0 ? temp > m.peak : temp < m.peak;
    if (further) {
      m.peak    = temp;
      m.peak_at = t;
    }
  }

  if (fabsf(error) <= m.band) {
    if (!m.in_band) {
      m.in_band   = true;
      m.settle_at = t;
    }
    if (!m.settled && t - m.settle_at >= SETTLE_CONFIRM_S) {
      m.settled       = true;
      m.settled_event = true;
    }
  } else {
    m.in_band   = false;
    m.settled   = false;
    m.settle_at = -1.0f;
  }
}

bool metrics_take_settled_event(void)
{
  bool e          = m.settled_event;
  m.settled_event = false;
  return e;
}

static const char *secs(char *buf, float s)
{
  if (s < 0) return "-";
  fmt_float(buf, 16, s, 1, false);
  return buf;
}

void metrics_print(const char *mode, Gains g, uint32_t now_ms)
{
  char a[16], b[16], c[16];

  console_printf("=== METRICS (%s) ===\r\n", mode);
  if (!m.active) {
    console_printf("No step recorded yet. Send START.\r\n");
    return;
  }

  console_printf("Step       : %s -> %s C, running %s s\r\n",
                 fmt_float(a, sizeof a, m.start_temp, 1, false),
                 fmt_float(b, sizeof b, m.setpoint, 1, false),
                 fmt_float(c, sizeof c, (float)(now_ms - m.t0) / 1000.0f, 1, false));

  bool big_enough = fabsf(m.step) >= MIN_STEP_C;

  if (!big_enough)
    console_printf("Rise 10-90 : n/a (step too small)\r\n");
  else if (m.rise_start >= 0 && m.rise_end >= 0)
    console_printf("Rise 10-90 : %s s\r\n", secs(a, m.rise_end - m.rise_start));
  else
    console_printf("Rise 10-90 : -\r\n");

  console_printf("Time to SP : %s s\r\n", secs(a, m.reached_at));

  float over = m.step > 0 ? m.peak - m.setpoint : m.setpoint - m.peak;
  if (!big_enough || over <= 0)
    console_printf("Overshoot  : 0.00 C\r\n");
  else
    console_printf("Overshoot  : %s C (%s %%) at %s s\r\n",
                   fmt_float(a, sizeof a, over, 2, false),
                   fmt_float(b, sizeof b, 100.0f * over / fabsf(m.step), 1, false),
                   secs(c, m.peak_at));

  fmt_float(a, sizeof a, m.band, 2, false);
  if (m.settled)
    console_printf("Settle +/-%s C: %s s\r\n", a, secs(b, m.settle_at));
  else if (m.in_band)
    console_printf("Settle +/-%s C: in band since %s s, confirming\r\n", a, secs(b, m.settle_at));
  else
    console_printf("Settle +/-%s C: not settled\r\n", a);

  console_printf("IAE        : %s C.s\r\n", fmt_float(a, sizeof a, m.iae, 1, false));
  console_printf("Gains      : Kp %s Ki %s Kd %s\r\n",
                 fmt_float(a, sizeof a, g.kp, 3, false),
                 fmt_float(b, sizeof b, g.ki, 4, false),
                 fmt_float(c, sizeof c, g.kd, 3, false));
}
