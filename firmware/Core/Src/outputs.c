#include "outputs.h"

#include "app_config.h"
#include "main.h"
#include "tim.h"

/* ---------------------------------------------------------------------------
 * Heater
 * ------------------------------------------------------------------------- */
static float heater_duty;

void heater_set(float pct)
{
  if (!(pct > 0.0f)) pct = 0.0f;  /* also catches NaN */
  if (pct > 100.0f) pct = 100.0f;
  heater_duty = pct;

  uint32_t top = __HAL_TIM_GET_AUTORELOAD(&htim1) + 1u;
  __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_2, (uint32_t)(pct * (float)top / 100.0f + 0.5f));
}

float heater_pct(void) { return heater_duty; }

/* ---------------------------------------------------------------------------
 * Fans
 * ------------------------------------------------------------------------- */
typedef struct {
  uint32_t channel;
  float    scale;
  float    onset;
  bool     on;
  uint32_t on_since;
  float    target;   /* % before scaling */
  float    current;  /* slewed, before scaling */
} Fan;

static Fan fans[2] = {
  {TIM_CHANNEL_1, FAN1_SCALE, FAN1_ONSET_C, false, 0, 0.0f, 0.0f},
  {TIM_CHANNEL_2, FAN2_SCALE, FAN2_ONSET_C, false, 0, 0.0f, 0.0f},
};
static uint32_t last_fan_update;

static float scaled(const Fan *f)
{
  float v = f->current * f->scale;
  return v > 100.0f ? 100.0f : v;
}

void fans_auto(float temp, float cool_above, uint32_t now_ms)
{
  float over = temp - cool_above;

  for (int i = 0; i < 2; i++) {
    Fan *f = &fans[i];
    if (!f->on) {
      if (over >= f->onset) {
        f->on = true;
        f->on_since = now_ms;
      }
    } else if (now_ms - f->on_since >= FAN_MIN_ON_MS && over < f->onset - FAN_HYST_C) {
      f->on = false;
    }

    if (f->on) {
      float r = (over - f->onset) / FAN_RAMP_RANGE_C;
      r = r < 0.0f ? 0.0f : (r > 1.0f ? 1.0f : r);
      f->target = FAN_MIN_PCT + r * (100.0f - FAN_MIN_PCT);
    } else {
      f->target = 0.0f;
    }
  }
}

void fans_full(uint32_t now_ms)
{
  for (int i = 0; i < 2; i++) {
    if (!fans[i].on) fans[i].on_since = now_ms;
    fans[i].on = true;
    fans[i].target = 100.0f;
  }
}

void fans_update(uint32_t now_ms)
{
  float dt = (float)(now_ms - last_fan_update) / 1000.0f;
  last_fan_update = now_ms;
  if (dt > 0.5f) dt = 0.5f;
  float step = FAN_SLEW_PCT_PER_S * dt;
  uint32_t top = __HAL_TIM_GET_AUTORELOAD(&htim3) + 1u;

  for (int i = 0; i < 2; i++) {
    Fan *f = &fans[i];
    if (f->current < f->target) {
      /* Jump straight past the stall threshold instead of crawling up to it. */
      if (f->current < FAN_MIN_PCT) f->current = f->target < FAN_MIN_PCT ? f->target : FAN_MIN_PCT;
      else f->current = (f->current + step > f->target) ? f->target : f->current + step;
    } else if (f->current > f->target) {
      f->current = (f->current - step < f->target) ? f->target : f->current - step;
      if (f->target == 0.0f && f->current < FAN_MIN_PCT) f->current = 0.0f;  /* below stall it just hums */
    }
    __HAL_TIM_SET_COMPARE(&htim3, f->channel, (uint32_t)(scaled(f) * (float)top / 100.0f + 0.5f));
  }
}

uint8_t fan_pct(uint8_t fan)
{
  if (fan < 1 || fan > 2) return 0;
  return (uint8_t)(scaled(&fans[fan - 1]) + 0.5f);
}

/* ---------------------------------------------------------------------------
 * Buzzer
 * ------------------------------------------------------------------------- */
typedef struct {
  uint16_t freq;  /* 0 = rest */
  uint16_t ms;
} Note;

static const Note n_startup[]   = {{1047, 120}, {0, 10}, {1319, 120}, {0, 10}, {1568, 150}};
static const Note n_ack[]       = {{784, 120}};
static const Note n_error[]     = {{200, 80}, {0, 40}, {200, 80}};
static const Note n_stop[]      = {{440, 200}};
static const Note n_at_start[]  = {{880, 300}};
static const Note n_at_step[]   = {{784, 80}};
static const Note n_at_done[]   = {{1319, 100}, {0, 20}, {1047, 100}, {0, 20}, {1319, 200}};
static const Note n_at_fail[]   = {{440, 150}, {0, 30}, {330, 250}};
static const Note n_fan_on[]    = {{784, 80}, {0, 10}, {1047, 80}};
static const Note n_fan_off[]   = {{1047, 80}, {0, 10}, {784, 80}};
static const Note n_overshoot[] = {{660, 200}};
static const Note n_fault[]     = {{200, 300}};

#define MELODY(x) {x, sizeof(x) / sizeof(x[0])}
static const struct {
  const Note *notes;
  uint8_t     count;
} melodies[SND_COUNT] = {
  [SND_STARTUP]        = MELODY(n_startup),
  [SND_ACK]            = MELODY(n_ack),
  [SND_ERROR]          = MELODY(n_error),
  [SND_STOP]           = MELODY(n_stop),
  [SND_AUTOTUNE_START] = MELODY(n_at_start),
  [SND_AUTOTUNE_STEP]  = MELODY(n_at_step),
  [SND_AUTOTUNE_DONE]  = MELODY(n_at_done),
  [SND_AUTOTUNE_FAIL]  = MELODY(n_at_fail),
  [SND_FANMODE_ON]     = MELODY(n_fan_on),
  [SND_FANMODE_OFF]    = MELODY(n_fan_off),
  [SND_OVERSHOOT]      = MELODY(n_overshoot),
  [SND_FAULT]          = MELODY(n_fault),
};
#undef MELODY

static int      playing = -1;
static uint8_t  note_index;
static uint32_t note_ends_at;
static uint8_t  volume = BUZZER_DEFAULT_VOLUME;

static void tone_off(void)
{
  HAL_TIM_Base_Stop_IT(&htim6);
  HAL_GPIO_WritePin(BUZZER_GPIO_Port, BUZZER_Pin, GPIO_PIN_RESET);
}

/* TIM6 ticks at 1 MHz; the pin toggles on every update, so the period is
 * half a cycle of the note. */
static void tone_on(uint16_t freq)
{
  HAL_TIM_Base_Stop_IT(&htim6);
  __HAL_TIM_SET_AUTORELOAD(&htim6, 1000000u / (2u * freq) - 1u);
  __HAL_TIM_SET_COUNTER(&htim6, 0);
  HAL_TIM_Base_Start_IT(&htim6);
}

static void start_note(uint32_t now)
{
  const Note *n  = &melodies[playing].notes[note_index];
  uint32_t    ms = n->ms;
  if (n->freq) {
    /* A passive buzzer has no amplitude control, so volume shortens the note. */
    ms = (uint32_t)n->ms * volume / 10u;
    if (ms < 10u) ms = 10u;
    tone_on(n->freq);
  } else {
    tone_off();
  }
  note_ends_at = now + ms;
}

void buzzer_play(Sound s)
{
  if (volume == 0 || s >= SND_COUNT) return;
  playing    = (int)s;
  note_index = 0;
  start_note(HAL_GetTick());
}

void buzzer_update(uint32_t now_ms)
{
  if (playing < 0 || (int32_t)(now_ms - note_ends_at) < 0) return;
  if (++note_index >= melodies[playing].count) {
    playing = -1;
    tone_off();
    return;
  }
  start_note(now_ms);
}

bool buzzer_busy(void) { return playing >= 0; }

void buzzer_set_volume(uint8_t v)
{
  volume = v > 10 ? 10 : v;
  if (volume == 0) {
    playing = -1;
    tone_off();
  }
}

uint8_t buzzer_volume(void) { return volume; }

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  if (htim == &htim6) HAL_GPIO_TogglePin(BUZZER_GPIO_Port, BUZZER_Pin);
}

/* ---------------------------------------------------------------------------
 * Start-up
 * ------------------------------------------------------------------------- */
void outputs_init(void)
{
  heater_set(0.0f);
  __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_1, 0);
  __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_2, 0);

  /* Fans spin one way only: the B side of each bridge stays low. */
  HAL_GPIO_WritePin(FAN1_DIR_GPIO_Port, FAN1_DIR_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(FAN2_DIR_GPIO_Port, FAN2_DIR_Pin, GPIO_PIN_RESET);

  HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_2);
  HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_1);
  HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_2);

  last_fan_update = HAL_GetTick();
  tone_off();
}
