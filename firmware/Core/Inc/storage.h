#ifndef STORAGE_H
#define STORAGE_H

#include <stdbool.h>
#include <stdint.h>

#include "controller.h"

/* Settings kept in the last 2 KB flash page (reserved in the linker script).
 * Records are appended until the page is full, so the page is only erased
 * every few dozen saves, and a write cut short by power loss just falls back
 * to the previous record. */

typedef struct {
  float   setpoint;
  uint8_t volume;
  uint8_t has_tune;
  uint8_t reserved[2];
  Gains   zn;
  float   ku, tu, amplitude, hold_pct;
} Settings;

void storage_defaults(Settings *s);
bool storage_load(Settings *s);        /* false: nothing valid stored, defaults filled in */
bool storage_save(const Settings *s);  /* skips the write if nothing changed */
bool storage_erase(void);

#endif
