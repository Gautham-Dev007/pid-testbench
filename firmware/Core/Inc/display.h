#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#include "controller.h"

typedef struct {
  float       temp;
  bool        temp_valid;
  float       setpoint;
  float       heater_pct;
  uint8_t     fan1_pct, fan2_pct;
  float       dtdt;
  Gains       gains;
  const char *mode_tag;   /* footer tag: MANUAL, ZN, ADAPT, TUNE, STOP, FAN, FAULT */
  bool        running;
  bool        autotuning;
  bool        fan_mode;
  bool        tuned;      /* ZN gains come from an autotune */
  uint8_t     tune_switches;
  int8_t      zone;       /* adaptive zone, -1 if not adaptive */
  bool        overshoot;
  bool        fault;
  const char *fault_title;
  const char *fault_detail;
} DisplayData;

void display_init(void);  /* includes the short splash screen */

/* Redraws the frame buffer. Three pages rotate every OLED_PAGE_MS. */
void display_render(const DisplayData *d, uint32_t now_ms);

/* Call every loop: pushes the frame out one page at a time. */
void display_service(void);

#endif
