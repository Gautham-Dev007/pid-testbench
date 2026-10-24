#include "max31865.h"

#include <math.h>

#include "app_config.h"
#include "main.h"
#include "spi.h"

#define REG_CONFIG   0x00u
#define REG_RTD_MSB  0x01u
#define REG_FAULT    0x07u
#define WRITE        0x80u

#define CFG_BIAS     0x80u
#define CFG_ONESHOT  0x20u
#define CFG_3WIRE    0x10u
#define CFG_CLEAR    0x02u
#define CFG_50HZ     0x01u

#define BIAS_SETTLE_MS 10u
#define CONVERT_MS     66u   /* 62.5 ms at 50 Hz filter, 52 ms at 60 Hz */
#define SPI_TIMEOUT_MS 5u

#define CVD_A 3.9083e-3f
#define CVD_B (-5.775e-7f)

static const uint8_t base_cfg = (RTD_3WIRE ? CFG_3WIRE : 0u) | (RTD_FILTER_50HZ ? CFG_50HZ : 0u);

typedef enum { ST_IDLE, ST_BIAS, ST_CONVERT } State;

static State    state;
static uint32_t phase_start, next_start;
static bool     started;

static bool write_reg(uint8_t reg, uint8_t value)
{
  uint8_t tx[2] = {(uint8_t)(reg | WRITE), value};
  HAL_GPIO_WritePin(RTD_CS_GPIO_Port, RTD_CS_Pin, GPIO_PIN_RESET);
  bool ok = HAL_SPI_Transmit(&hspi1, tx, 2, SPI_TIMEOUT_MS) == HAL_OK;
  HAL_GPIO_WritePin(RTD_CS_GPIO_Port, RTD_CS_Pin, GPIO_PIN_SET);
  return ok;
}

static bool read_regs(uint8_t reg, uint8_t *out, uint16_t n)
{
  uint8_t addr = reg & 0x7Fu;
  HAL_GPIO_WritePin(RTD_CS_GPIO_Port, RTD_CS_Pin, GPIO_PIN_RESET);
  bool ok = HAL_SPI_Transmit(&hspi1, &addr, 1, SPI_TIMEOUT_MS) == HAL_OK &&
            HAL_SPI_Receive(&hspi1, out, n, SPI_TIMEOUT_MS) == HAL_OK;
  HAL_GPIO_WritePin(RTD_CS_GPIO_Port, RTD_CS_Pin, GPIO_PIN_SET);
  return ok;
}

/* Callendar-Van Dusen. Above 0 degC the quadratic is solved exactly; below it
 * a fitted polynomial is accurate to a few hundredths of a degree. */
static float rtd_to_celsius(uint16_t code)
{
  float r = (float)code / 32768.0f * RTD_RREF;

  float t = (-CVD_A + sqrtf(CVD_A * CVD_A - 4.0f * CVD_B * (1.0f - r / RTD_NOMINAL))) / (2.0f * CVD_B);
  if (t >= 0.0f) return t;

  float x = r / RTD_NOMINAL * 100.0f;
  return -242.02f + x * (2.2228f + x * (2.5859e-3f + x * (-4.8260e-6f + x * (-2.8183e-8f + x * 1.5243e-10f))));
}

void max31865_init(void)
{
  HAL_GPIO_WritePin(RTD_CS_GPIO_Port, RTD_CS_Pin, GPIO_PIN_SET);
  write_reg(REG_CONFIG, base_cfg | CFG_CLEAR);
  state   = ST_IDLE;
  started = false;
}

bool max31865_poll(uint32_t now, uint32_t period_ms, RtdSample *out)
{
  switch (state) {
    case ST_IDLE:
      if (started && (int32_t)(now - next_start) < 0) return false;
      /* Keep a fixed cadence, but don't try to catch up after a long stall. */
      next_start = (started && now - next_start < period_ms) ? next_start + period_ms : now + period_ms;
      started = true;
      phase_start = now;
      write_reg(REG_CONFIG, base_cfg | CFG_BIAS);
      state = ST_BIAS;
      return false;

    case ST_BIAS:
      if (now - phase_start < BIAS_SETTLE_MS) return false;
      write_reg(REG_CONFIG, base_cfg | CFG_BIAS | CFG_ONESHOT);
      phase_start = now;
      state = ST_CONVERT;
      return false;

    case ST_CONVERT: {
      if (now - phase_start < CONVERT_MS) return false;

      uint8_t cfg = 0, rtd[2] = {0, 0}, status = 0;
      bool ok = read_regs(REG_CONFIG, &cfg, 1) &&
                read_regs(REG_RTD_MSB, rtd, 2) &&
                read_regs(REG_FAULT, &status, 1);

      out->taken_ms = phase_start;
      out->fault    = status;
      /* The one-shot bit clears itself, so the rest must read back as written.
       * A missing board reads all zeros or all ones and fails this. */
      if (!ok || (cfg & (uint8_t)~CFG_ONESHOT) != (base_cfg | CFG_BIAS))
        out->fault |= RTD_FAULT_COMMS;

      uint16_t code = (uint16_t)(((uint16_t)rtd[0] << 8 | rtd[1]) >> 1);
      out->temp_c   = rtd_to_celsius(code);
      if (isnan(out->temp_c) || out->temp_c < RTD_MIN_VALID_C || out->temp_c > RTD_MAX_VALID_C)
        out->fault |= RTD_FAULT_RANGE;

      write_reg(REG_CONFIG, base_cfg | (status ? CFG_CLEAR : 0u));  /* bias off */
      state = ST_IDLE;
      return true;
    }
  }
  return false;
}

const char *max31865_fault_text(uint8_t fault)
{
  if (fault & RTD_FAULT_COMMS)       return "No reply from MAX31865";
  if (fault & RTD_FAULT_HIGH_THRESH) return "RTD high / open";
  if (fault & RTD_FAULT_LOW_THRESH)  return "RTD low / short";
  if (fault & RTD_FAULT_REFIN_HIGH)  return "REFIN- high";
  if (fault & RTD_FAULT_REFIN_LOW)   return "FORCE- open";
  if (fault & RTD_FAULT_RTDIN_LOW)   return "RTDIN- open";
  if (fault & RTD_FAULT_VOLTAGE)     return "Over/undervoltage";
  if (fault & RTD_FAULT_RANGE)       return "Reading out of range";
  return "OK";
}
