#include "app.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "autotune.h"
#include "commands.h"
#include "console.h"
#include "display.h"
#include "fmt.h"
#include "iwdg.h"
#include "main.h"
#include "max31865.h"
#include "metrics.h"
#include "outputs.h"
#include "storage.h"

#define RENDER_PERIOD_MS 200u
#define BEEP_REPEAT_MS   2000u

typedef enum { ST_IDLE, ST_RUNNING, ST_AUTOTUNE, ST_FANMODE, ST_FAULT } State;
typedef enum { FAULT_NONE, FAULT_SENSOR, FAULT_OVERTEMP, FAULT_RUNAWAY } Fault;

static State state = ST_IDLE;
static State state_before_fans;
static Fault fault;

static Settings settings;
static bool     settings_dirty;
static uint32_t settings_changed_at;

static float    setpoint;
static float    temp;           /* last good reading */
static bool     temp_valid;
static uint8_t  bad_samples;
static uint8_t  sensor_bits;    /* fault bits of the last bad sample */
static float    dtdt;
static uint32_t last_sample_ms;
static uint32_t last_control_ms;
static bool     control_primed;

static bool     runaway_tracking;
static uint32_t runaway_since;
static float    runaway_start_temp;

static bool     overshoot;
static uint32_t last_alarm_beep;
static uint32_t last_render;
static uint8_t  last_tune_switches;
static bool     start_pending = START_ON_BOOT;

/* ---------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

static const char *f2s(char *buf, float v, uint8_t decimals)
{
  return fmt_float(buf, 16, v, decimals, false);
}

static const char *state_label(void)
{
  switch (state) {
    case ST_IDLE:     return "STOPPED";
    case ST_AUTOTUNE: return "AUTOTUNE";
    case ST_FANMODE:  return "FANMODE";
    case ST_FAULT:    return "FAULT";
    case ST_RUNNING:  break;
  }
  return ctrl_mode_name();
}

static const char *fault_title(void)
{
  switch (fault) {
    case FAULT_SENSOR:   return "Sensor fault";
    case FAULT_OVERTEMP: return "Over-temperature";
    case FAULT_RUNAWAY:  return "No heat rise";
    case FAULT_NONE:     break;
  }
  return "";
}

static const char *fault_detail(void)
{
  static char buf[24];
  switch (fault) {
    case FAULT_SENSOR:
      return max31865_fault_text(sensor_bits);
    case FAULT_OVERTEMP: {
      char a[16];
      snprintf(buf, sizeof buf, "Limit %s C", f2s(a, OVERTEMP_TRIP_C, 0));
      return buf;
    }
    case FAULT_RUNAWAY:
      return "Check heater/sensor";
    case FAULT_NONE:
      break;
  }
  return "";
}

static bool fault_cleared(void)
{
  switch (fault) {
    case FAULT_SENSOR:   return temp_valid && bad_samples == 0;
    case FAULT_OVERTEMP: return temp_valid && bad_samples == 0 && temp < OVERTEMP_CLEAR_C;
    default:             return true;
  }
}

static void mark_settings_dirty(void)
{
  settings_dirty      = true;
  settings_changed_at = HAL_GetTick();
}

static void save_settings_now(void)
{
  settings_dirty = false;
  if (!storage_save(&settings)) console_printf("ERR could not write settings to flash\r\n");
}

static bool gains_sane(Gains g)
{
  return isfinite(g.kp) && isfinite(g.ki) && isfinite(g.kd) &&
         g.kp > 0 && g.kp < 1000 && g.ki >= 0 && g.ki < 1000 && g.kd >= 0 && g.kd < 1000;
}

static void begin_step(uint32_t now)
{
  metrics_start(setpoint, temp, now);
  control_primed = false;
}

static void enter_fault(Fault f, uint32_t now)
{
  if (state == ST_AUTOTUNE) tune_abort();
  heater_set(0.0f);
  fault            = f;
  state            = ST_FAULT;
  runaway_tracking = false;
  last_alarm_beep  = now;
  buzzer_play(SND_FAULT);
  console_printf("FAULT %s: %s. Heater off, send STOP to clear.\r\n", fault_title(), fault_detail());
}

/* ---------------------------------------------------------------------------
 * Telemetry: one line per control cycle, parsed by the dashboard.
 *   SP:75.0 T:23.45C D:0% M:ZN Kp:10.000 Ki:2.0000 F1:0% F2:0% dT/dt:0.00 Kd:1.500
 * ------------------------------------------------------------------------- */
static void send_telemetry(bool sample_ok)
{
  char sp[16], t[16], kp[16], ki[16], rate[16], kd[16];
  Gains g = ctrl_active_gains();

  console_printf("SP:%s T:%sC D:%d%% M:%s Kp:%s Ki:%s F1:%d%% F2:%d%% dT/dt:%s Kd:%s\r\n",
                 f2s(sp, setpoint, 1),
                 sample_ok ? f2s(t, temp, 2) : "nan",
                 (int)(heater_pct() + 0.5f),
                 state_label(),
                 f2s(kp, g.kp, 3), f2s(ki, g.ki, 4),
                 fan_pct(1), fan_pct(2),
                 fmt_float(rate, sizeof rate, dtdt, 2, false),
                 f2s(kd, g.kd, 3));
}

/* ---------------------------------------------------------------------------
 * Autotune hand-over
 * ------------------------------------------------------------------------- */
static void finish_autotune(TuneStatus st, uint32_t now)
{
  char a[16], b[16], c[16], d[16];

  if (st == TUNE_FAILED) {
    state = ST_IDLE;
    heater_set(0.0f);
    buzzer_play(SND_AUTOTUNE_FAIL);
    console_printf("Autotune FAILED (%s), gains unchanged.\r\n", tune_fail_reason());
    return;
  }

  const TuneResult *r = tune_result();
  console_printf("Autotune done in %lu s: amplitude %s C, Tu %s s, hold %s %%\r\n",
                 (unsigned long)tune_elapsed_s(now), f2s(a, r->amplitude, 2), f2s(b, r->tu, 1),
                 f2s(c, r->hold_pct, 1));

  if (!gains_sane(r->gains)) {
    state = ST_IDLE;
    heater_set(0.0f);
    buzzer_play(SND_AUTOTUNE_FAIL);
    console_printf("Autotune FAILED (gains out of range), gains unchanged.\r\n");
    return;
  }

  console_printf("Ku %s -> Kp %s Ki %s Kd %s (ZN rule %d)\r\n", f2s(a, r->ku, 3), f2s(b, r->gains.kp, 3),
                 f2s(c, r->gains.ki, 4), f2s(d, r->gains.kd, 3), ZN_RULE);

  ctrl_set_tuned_gains(r->gains);
  settings.has_tune  = 1;
  settings.zn        = r->gains;
  settings.ku        = r->ku;
  settings.tu        = r->tu;
  settings.amplitude = r->amplitude;
  settings.hold_pct  = r->hold_pct;
  save_settings_now();

  /* Bumpless hand-over: start the integrator at the measured hold power. */
  ctrl_set_mode(MODE_ZN);
  ctrl_reset();
  ctrl_preload_integral(r->hold_pct);
  begin_step(now);
  state = ST_RUNNING;
  buzzer_play(SND_AUTOTUNE_DONE);
  console_printf("ZN control running.\r\n");
}

/* ---------------------------------------------------------------------------
 * Control cycle, once per RTD sample
 * ------------------------------------------------------------------------- */
static void control_cycle(const RtdSample *s, uint32_t now)
{
  bool ok = s->fault == 0;
  if (ok && temp_valid && fabsf(s->temp_c - temp) > RTD_MAX_STEP_C) ok = false;  /* glitch */

  if (ok) {
    float dt = (float)(now - last_sample_ms) / 1000.0f;
    if (temp_valid && dt > 0.0f && dt < 1.0f) dtdt += dt / (1.0f + dt) * ((s->temp_c - temp) / dt - dtdt);
    else dtdt = 0.0f;
    temp           = s->temp_c;
    temp_valid     = true;
    bad_samples    = 0;
    last_sample_ms = now;
  } else {
    sensor_bits = s->fault ? s->fault : RTD_FAULT_RANGE;
    if (bad_samples < 255) bad_samples++;
    if (bad_samples >= SENSOR_FAULT_TRIP) {
      temp_valid = false;
      if (state != ST_FAULT) enter_fault(FAULT_SENSOR, now);
    }
    /* Never act on a reading we don't trust. */
    heater_set(0.0f);
    control_primed = false;
    if (state == ST_FAULT) fans_full(now);
    send_telemetry(false);
    return;
  }

  if (temp >= OVERTEMP_TRIP_C && state != ST_FAULT) enter_fault(FAULT_OVERTEMP, now);

  bool heating = state == ST_RUNNING || state == ST_AUTOTUNE;
  if (RUNAWAY_WINDOW_S && heating && heater_pct() >= RUNAWAY_DUTY_PCT && temp < setpoint - RUNAWAY_BELOW_SP_C) {
    if (!runaway_tracking) {
      runaway_tracking   = true;
      runaway_since      = now;
      runaway_start_temp = temp;
    } else if (now - runaway_since >= RUNAWAY_WINDOW_S * 1000u) {
      if (temp - runaway_start_temp < RUNAWAY_MIN_RISE_C) {
        enter_fault(FAULT_RUNAWAY, now);
      } else {
        runaway_since      = now;
        runaway_start_temp = temp;
      }
    }
  } else {
    runaway_tracking = false;
  }

  if (start_pending) {
    start_pending = false;
    app_cmd_start();
  }

  float out = 0.0f;
  switch (state) {
    case ST_RUNNING: {
      float dt = control_primed ? (float)(now - last_control_ms) / 1000.0f : 0.0f;
      out = ctrl_update(setpoint, temp, dt);
      metrics_update(setpoint, temp, dt, now);
      if (metrics_take_settled_event()) {
        console_printf("SETTLED\r\n");
        metrics_print(ctrl_mode_name(), ctrl_active_gains(), now);
      }
      break;
    }
    case ST_AUTOTUNE: {
      TuneStatus st = tune_update(temp, setpoint, now);
      if (tune_switches() != last_tune_switches) {
        last_tune_switches = tune_switches();
        char a[16];
        console_printf("Relay switch %d/%d at %s C\r\n", last_tune_switches, RELAY_SWITCHES, f2s(a, temp, 2));
        if (st == TUNE_RUNNING) buzzer_play(SND_AUTOTUNE_STEP);
      }
      if (st == TUNE_RUNNING) out = tune_heater_pct();
      else finish_autotune(st, now);
      break;
    }
    default:
      break;
  }
  last_control_ms = now;
  control_primed  = state == ST_RUNNING;
  heater_set(out);

  if (state == ST_FAULT || state == ST_FANMODE) fans_full(now);
  else fans_auto(temp, state == ST_AUTOTUNE ? setpoint + AUTOTUNE_FAN_SHIFT_C : setpoint, now);

  bool was_over = overshoot;
  overshoot = temp > setpoint + OVERSHOOT_WARN_C;
  if (overshoot && !was_over) {
    char a[16];
    console_printf("WARNING overshoot, more than %s C above SP\r\n", f2s(a, OVERSHOOT_WARN_C, 1));
  }

  if (now - last_alarm_beep >= BEEP_REPEAT_MS && !buzzer_busy()) {
    if (state == ST_FAULT) {
      buzzer_play(SND_FAULT);
      last_alarm_beep = now;
    } else if (overshoot && heating) {
      buzzer_play(SND_OVERSHOOT);
      last_alarm_beep = now;
    }
  }

  send_telemetry(true);
}

static void render(uint32_t now)
{
  DisplayData d = {
    .temp          = temp,
    .temp_valid    = temp_valid && bad_samples == 0,
    .setpoint      = setpoint,
    .heater_pct    = heater_pct(),
    .fan1_pct      = fan_pct(1),
    .fan2_pct      = fan_pct(2),
    .dtdt          = dtdt,
    .gains         = ctrl_active_gains(),
    .running       = state == ST_RUNNING || state == ST_AUTOTUNE,
    .autotuning    = state == ST_AUTOTUNE,
    .fan_mode      = state == ST_FANMODE,
    .tuned         = ctrl_has_tuned_gains() && ctrl_mode() == MODE_ZN,
    .tune_switches = tune_switches(),
    .zone          = ctrl_mode() == MODE_ADAPTIVE ? ctrl_zone() : -1,
    .overshoot     = overshoot,
    .fault         = state == ST_FAULT,
    .fault_title   = fault_title(),
    .fault_detail  = fault_detail(),
  };
  switch (state) {
    case ST_IDLE:     d.mode_tag = "STOP"; break;
    case ST_AUTOTUNE: d.mode_tag = "TUNE"; break;
    case ST_FANMODE:  d.mode_tag = "FAN"; break;
    case ST_FAULT:    d.mode_tag = "FAULT"; break;
    case ST_RUNNING:  d.mode_tag = ctrl_mode_name(); break;
  }
  display_render(&d, now);
}

/* ---------------------------------------------------------------------------
 * Commands
 * ------------------------------------------------------------------------- */

void app_cmd_start(void)
{
  uint32_t now = HAL_GetTick();
  if (state == ST_FAULT && !fault_cleared()) {
    buzzer_play(SND_ERROR);
    console_printf("ERR fault still present: %s\r\n", fault_detail());
    return;
  }
  if (!temp_valid) {
    buzzer_play(SND_ERROR);
    console_printf("ERR no valid temperature yet\r\n");
    return;
  }
  if (state == ST_AUTOTUNE) {
    tune_abort();
    console_printf("Autotune aborted.\r\n");
  }

  char a[16];
  fault = FAULT_NONE;
  ctrl_reset();
  begin_step(now);
  state = ST_RUNNING;
  buzzer_play(SND_ACK);
  console_printf("Control started: %s, SP %s C\r\n", ctrl_mode_name(), f2s(a, setpoint, 1));
  if (ctrl_mode() == MODE_ZN && !ctrl_has_tuned_gains())
    console_printf("Note: no autotune data, ZN is using the manual gains. Run AUTOTUNE.\r\n");
}

void app_cmd_stop(void)
{
  if (state == ST_FAULT) {
    if (!fault_cleared()) {
      buzzer_play(SND_ERROR);
      console_printf("ERR fault still present: %s\r\n", fault_detail());
      return;
    }
    fault = FAULT_NONE;
    console_printf("Fault cleared.\r\n");
  }
  if (state == ST_AUTOTUNE) tune_abort();
  state = ST_IDLE;
  heater_set(0.0f);
  buzzer_play(SND_STOP);
  console_printf("Stopped.\r\n");
}

void app_cmd_autotune(void)
{
  char a[16], b[16];
  if (state == ST_FAULT) {
    console_printf("ERR clear the fault first (STOP)\r\n");
    return;
  }
  if (state == ST_AUTOTUNE) {
    console_printf("Autotune already running (%d/%d switches).\r\n", tune_switches(), RELAY_SWITCHES);
    return;
  }
  if (!temp_valid) {
    console_printf("ERR no valid temperature yet\r\n");
    return;
  }
  tune_start(HAL_GetTick());
  last_tune_switches = 0;
  state = ST_AUTOTUNE;
  buzzer_play(SND_AUTOTUNE_START);
  console_printf("Autotune started at SP %s C. Relay +/-%s C, %d switches, timeout %u min.\r\n",
                 f2s(a, setpoint, 1), f2s(b, RELAY_HYST_C, 1), RELAY_SWITCHES, (unsigned)(AUTOTUNE_TIMEOUT_S / 60u));
}

void app_cmd_fanmode(int on)
{
  bool active = state == ST_FANMODE;
  if (on < 0) on = !active;

  if (on && !active) {
    if (state == ST_FAULT) {
      console_printf("Fans are already at full speed while the fault is active.\r\n");
      return;
    }
    if (state == ST_AUTOTUNE) {
      tune_abort();
      console_printf("Autotune aborted.\r\n");
    }
    state_before_fans = state == ST_RUNNING ? ST_RUNNING : ST_IDLE;
    state = ST_FANMODE;
    heater_set(0.0f);
    buzzer_play(SND_FANMODE_ON);
    console_printf("FANMODE on: fans full, heater locked off.\r\n");
  } else if (!on && active) {
    state = state_before_fans;
    if (state == ST_RUNNING) {
      ctrl_reset();
      begin_step(HAL_GetTick());
    }
    buzzer_play(SND_FANMODE_OFF);
    console_printf("FANMODE off: %s.\r\n", state == ST_RUNNING ? "control resumed" : "stopped");
  } else {
    console_printf("FANMODE already %s.\r\n", active ? "on" : "off");
  }
}

void app_cmd_reset(void)
{
  ctrl_reset();
  if (state == ST_RUNNING) begin_step(HAL_GetTick());
  buzzer_play(SND_ACK);
  console_printf("Integrator cleared, metrics restarted.\r\n");
}

void app_cmd_setpoint(float value)
{
  char a[16], b[16];
  if (state == ST_AUTOTUNE) {
    console_printf("ERR stop the autotune before changing the setpoint\r\n");
    return;
  }
  if (value < SETPOINT_MIN || value > SETPOINT_MAX) {
    buzzer_play(SND_ERROR);
    console_printf("ERR setpoint must be %s..%s C\r\n", f2s(a, SETPOINT_MIN, 0), f2s(b, SETPOINT_MAX, 0));
    return;
  }
  setpoint = value;
  settings.setpoint = value;
  mark_settings_dirty();
  if (state == ST_RUNNING) metrics_start(setpoint, temp, HAL_GetTick());
  buzzer_play(SND_ACK);
  console_printf("SP -> %s C\r\n", f2s(a, setpoint, 1));
}

void app_cmd_mode(int mode)
{
  if (state == ST_AUTOTUNE) {
    console_printf("ERR stop the autotune before changing mode\r\n");
    return;
  }
  ctrl_set_mode((ControlMode)mode);
  if (state == ST_RUNNING) begin_step(HAL_GetTick());
  buzzer_play(SND_ACK);
  console_printf("Mode -> %s\r\n", ctrl_mode_name());
  if (mode == MODE_ZN && !ctrl_has_tuned_gains())
    console_printf("Note: no autotune data, ZN is using the manual gains. Run AUTOTUNE.\r\n");
}

void app_cmd_set_gains(Gains g)
{
  char a[16], b[16], c[16];
  if (state == ST_AUTOTUNE) {
    console_printf("ERR stop the autotune first\r\n");
    return;
  }
  ctrl_set_manual_gains(g);
  ctrl_set_mode(MODE_MANUAL);
  if (state == ST_RUNNING) begin_step(HAL_GetTick());
  buzzer_play(SND_ACK);
  console_printf("MANUAL gains -> Kp %s Ki %s Kd %s (kept until reboot)\r\n",
                 f2s(a, g.kp, 3), f2s(b, g.ki, 4), f2s(c, g.kd, 3));
}

void app_cmd_gains(void)
{
  char a[16], b[16], c[16];
  Gains g = ctrl_active_gains();
  console_printf("Kp:%s Ki:%s Kd:%s (%s)\r\n", f2s(a, g.kp, 4), f2s(b, g.ki, 4), f2s(c, g.kd, 4), ctrl_mode_name());
  if (ctrl_has_tuned_gains()) {
    Gains t = ctrl_tuned_gains();
    console_printf("Autotuned: Kp %s Ki %s Kd %s\r\n", f2s(a, t.kp, 4), f2s(b, t.ki, 4), f2s(c, t.kd, 4));
  } else {
    console_printf("Autotuned: none\r\n");
  }
}

void app_cmd_metrics(void) { metrics_print(ctrl_mode_name(), ctrl_active_gains(), HAL_GetTick()); }

void app_cmd_status(void)
{
  char a[16], b[16], c[16];
  console_printf("State   : %s%s%s\r\n", state_label(), state == ST_FAULT ? " - " : "",
                 state == ST_FAULT ? fault_title() : "");
  console_printf("Temp    : %s C   SP %s C   heater %s %%\r\n",
                 temp_valid ? f2s(a, temp, 2) : "nan", f2s(b, setpoint, 1), f2s(c, heater_pct(), 1));
  console_printf("Sensor  : %s\r\n", bad_samples ? max31865_fault_text(sensor_bits) : "OK");
  console_printf("Fans    : %d %% / %d %%\r\n", fan_pct(1), fan_pct(2));
  console_printf("Mode    : %s\r\n", ctrl_mode_name());
  if (settings.has_tune)
    console_printf("Tune    : Ku %s Tu %s s hold %s %%\r\n", f2s(a, settings.ku, 3), f2s(b, settings.tu, 1),
                   f2s(c, settings.hold_pct, 1));
  console_printf("Volume  : %d\r\n", buzzer_volume());
  console_printf("Uptime  : %lu s\r\n", (unsigned long)(HAL_GetTick() / 1000u));
}

void app_cmd_volume(int volume)
{
  if (volume < 0 || volume > 10) {
    console_printf("ERR volume must be 0..10\r\n");
    return;
  }
  buzzer_set_volume((uint8_t)volume);
  settings.volume = (uint8_t)volume;
  mark_settings_dirty();
  buzzer_play(SND_ACK);
  console_printf("Volume -> %d\r\n", volume);
}

void app_cmd_factory_reset(void)
{
  if (state == ST_AUTOTUNE) tune_abort();
  if (state != ST_FAULT) state = ST_IDLE;
  heater_set(0.0f);

  storage_erase();
  storage_defaults(&settings);
  settings_dirty = false;
  setpoint = settings.setpoint;
  buzzer_set_volume(settings.volume);
  ctrl_init();
  buzzer_play(SND_STOP);
  console_printf("Settings erased. Defaults restored, autotune data cleared.\r\n");
}

/* ---------------------------------------------------------------------------
 * Entry points
 * ------------------------------------------------------------------------- */

void app_init(void)
{
  bool watchdog_reset = __HAL_RCC_GET_FLAG(RCC_FLAG_IWDGRST) != 0;
  __HAL_RCC_CLEAR_RESET_FLAGS();

  outputs_init();
  console_init();

  console_printf("\r\n=== IEEE SPS x WIE PID Testbench (STM32G070RB) ===\r\n");
  if (watchdog_reset) console_printf("WARNING: restarted by the watchdog\r\n");

  bool stored = storage_load(&settings);
  if (!(settings.setpoint >= SETPOINT_MIN && settings.setpoint <= SETPOINT_MAX)) settings.setpoint = SETPOINT_DEFAULT;
  if (settings.volume > 10) settings.volume = BUZZER_DEFAULT_VOLUME;
  if (settings.has_tune && !gains_sane(settings.zn)) settings.has_tune = 0;

  setpoint = settings.setpoint;
  buzzer_set_volume(settings.volume);
  ctrl_init();
  if (settings.has_tune) ctrl_set_tuned_gains(settings.zn);

  max31865_init();
  display_init();
  buzzer_play(SND_STARTUP);

  char a[16];
  console_printf("Settings: %s, SP %s C, mode %s, %s\r\n", stored ? "loaded" : "defaults",
                 f2s(a, setpoint, 1), ctrl_mode_name(), settings.has_tune ? "autotune data present" : "no autotune data");
  commands_print_help();
  console_printf(START_ON_BOOT ? "Starting control...\r\n" : "Idle. Send START to begin.\r\n");

  last_render = HAL_GetTick();
}

void app_loop(void)
{
  uint32_t now = HAL_GetTick();
  HAL_IWDG_Refresh(&hiwdg);

  char line[64];
  while (console_read_line(line, sizeof line)) commands_execute(line);

  buzzer_update(now);
  fans_update(now);

  RtdSample sample;
  if (max31865_poll(now, CONTROL_PERIOD_MS, &sample)) control_cycle(&sample, now);

  if (now - last_render >= RENDER_PERIOD_MS) {
    last_render = now;
    render(now);
  }
  display_service();

  if (settings_dirty && now - settings_changed_at >= SETPOINT_SAVE_DELAY_MS) save_settings_now();
}
