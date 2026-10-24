#ifndef OUTPUTS_H
#define OUTPUTS_H

#include <stdbool.h>
#include <stdint.h>

/* Heater, fans and buzzer. Nothing here decides when to heat; the state
 * machine in app.c calls heater_set() exactly once per control cycle. */

void outputs_init(void);

/* Heater: TIM1_CH2 at 20 kHz. The channel polarity is set to active-low in
 * CubeMX, so a compare value of 0 keeps the SSR off. */
void  heater_set(float pct);
float heater_pct(void);

/* Fans: TIM3_CH1/CH2. Automatic cooling starts once temp passes
 * cool_above + onset. fans_update() slews towards the targets. */
void    fans_auto(float temp, float cool_above, uint32_t now_ms);
void    fans_full(uint32_t now_ms);
void    fans_update(uint32_t now_ms);
uint8_t fan_pct(uint8_t fan);  /* 1 or 2, actual output after scaling */

typedef enum {
  SND_STARTUP,
  SND_ACK,
  SND_ERROR,
  SND_STOP,
  SND_AUTOTUNE_START,
  SND_AUTOTUNE_STEP,
  SND_AUTOTUNE_DONE,
  SND_AUTOTUNE_FAIL,
  SND_FANMODE_ON,
  SND_FANMODE_OFF,
  SND_OVERSHOOT,
  SND_FAULT,
  SND_COUNT
} Sound;

/* Passive buzzer driven by toggling the pin from TIM6. Non-blocking. */
void    buzzer_play(Sound s);
void    buzzer_update(uint32_t now_ms);
bool    buzzer_busy(void);
void    buzzer_set_volume(uint8_t volume);  /* 0..10, 0 mutes */
uint8_t buzzer_volume(void);

#endif
