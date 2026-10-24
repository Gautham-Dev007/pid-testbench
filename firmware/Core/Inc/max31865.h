#ifndef MAX31865_H
#define MAX31865_H

#include <stdbool.h>
#include <stdint.h>

/* MAX31865 status bits, plus two of our own (bits 1 and 0 are unused by
 * the chip). Zero means the sample is usable. */
#define RTD_FAULT_HIGH_THRESH 0x80u
#define RTD_FAULT_LOW_THRESH  0x40u
#define RTD_FAULT_REFIN_HIGH  0x20u
#define RTD_FAULT_REFIN_LOW   0x10u
#define RTD_FAULT_RTDIN_LOW   0x08u
#define RTD_FAULT_VOLTAGE     0x04u
#define RTD_FAULT_COMMS       0x02u   /* config didn't read back: board missing */
#define RTD_FAULT_RANGE       0x01u   /* converted value is outside the valid window */

typedef struct {
  float    temp_c;
  uint8_t  fault;
  uint32_t taken_ms;  /* when the conversion was triggered */
} RtdSample;

void max31865_init(void);

/* Non-blocking. Runs one bias -> one-shot -> read sequence every period_ms
 * and returns true when a new sample is in *out. The bias is switched off
 * between conversions to keep self-heating down. */
bool max31865_poll(uint32_t now_ms, uint32_t period_ms, RtdSample *out);

const char *max31865_fault_text(uint8_t fault);

#endif
