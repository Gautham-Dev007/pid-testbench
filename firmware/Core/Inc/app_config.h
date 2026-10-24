#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/* Everything you might want to tune lives in this file. */

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Sensor: PT100 on a MAX31865, 3-wire
 * ------------------------------------------------------------------------- */
#define RTD_RREF             430.0f
#define RTD_NOMINAL          100.0f
#define RTD_3WIRE            1        /* 0 for 2- or 4-wire */
#define RTD_FILTER_50HZ      1        /* mains notch: 1 = 50 Hz, 0 = 60 Hz */

/* Readings outside this window are an open or shorted RTD, not a temperature. */
#define RTD_MIN_VALID_C      (-40.0f)
#define RTD_MAX_VALID_C      400.0f

/* A jump bigger than this between two samples is treated as a glitch. */
#define RTD_MAX_STEP_C       15.0f

/* Consecutive bad samples before the sensor fault latches (0.5 s). */
#define SENSOR_FAULT_TRIP    5

/* ---------------------------------------------------------------------------
 * Limits and safety
 * ------------------------------------------------------------------------- */
#define SETPOINT_MIN         20.0f
#define SETPOINT_MAX         150.0f
#define SETPOINT_DEFAULT     75.0f

/* Hard cutoff, independent of the setpoint. Latches until it cools. */
#define OVERTEMP_TRIP_C      160.0f
#define OVERTEMP_CLEAR_C     150.0f

#define OVERSHOOT_WARN_C     5.0f     /* OLED flash + beep above SP + this */

/* Heater runaway check: heater above RUNAWAY_DUTY_PCT for RUNAWAY_WINDOW_S
 * while more than RUNAWAY_BELOW_SP_C under the setpoint, yet the temperature
 * rose less than RUNAWAY_MIN_RISE_C. Usually a sensor that has come off the
 * block or a heater that isn't connected. 0 disables it. */
#define RUNAWAY_DUTY_PCT     80.0f
#define RUNAWAY_BELOW_SP_C   10.0f
#define RUNAWAY_MIN_RISE_C   1.5f
#define RUNAWAY_WINDOW_S     180u

/* The bench powers up idle and waits for START. */
#define START_ON_BOOT        0

/* ---------------------------------------------------------------------------
 * Timing
 * ------------------------------------------------------------------------- */
#define CONTROL_PERIOD_MS    100u     /* one RTD conversion per period */
#define OLED_PAGE_MS         4000u
#define SETPOINT_SAVE_DELAY_MS 10000u /* flash write once changes stop */

/* ---------------------------------------------------------------------------
 * Control
 * ------------------------------------------------------------------------- */
typedef enum { MODE_MANUAL = 0, MODE_ZN = 1, MODE_ADAPTIVE = 2 } ControlMode;

#define START_MODE           MODE_ADAPTIVE

/* Baseline for MANUAL, and for ZN until an autotune has run. */
#define MANUAL_KP            10.0f
#define MANUAL_KI            2.0f
#define MANUAL_KD            1.5f

/* Integration only runs within this many degrees below SP (it always runs
 * above SP, to unwind). Widened to PID_PBAND_PCT / Kp so a low-gain loop can
 * still remove its proportional offset. */
#define PID_INTEGRAL_BAND_C      8.0f
#define ADAPTIVE_INTEGRAL_BAND_C 5.0f
#define PID_PBAND_PCT            40.0f

#define PID_UNWIND           3.0f     /* the heater can't cool, so unwind faster */
#define PID_D_FILTER_TAU_S   0.9f
#define PID_D_LIMIT_PCT      40.0f

/* Adaptive gain schedule, far to near. A zone applies when |error| is above
 * its threshold; the last zone is the fallback. */
#define ADAPTIVE_ZONES_INIT  { \
    { 15.0f, 20.0f, 1.0f },   /* FAR:  fast pull-in        */ \
    {  5.0f, 12.0f, 2.0f },   /* MID:  balanced            */ \
    {  0.0f,  6.0f, 3.5f },   /* NEAR: tight, low overshoot */ \
  }
#define ADAPTIVE_ZONE_COUNT  3
#define ADAPTIVE_KD          1.5f
#define ADAPTIVE_ZONE_HYST   0.5f     /* stops gains chattering on a boundary */

/* Ziegler-Nichols rule applied to the relay result.
 *   0  classic PID        Kp 0.60 Ku   Ti Tu/2    Td Tu/8
 *   1  no-overshoot PID   Kp 0.20 Ku   Ti Tu/2    Td Tu/3   (default)
 *   2  classic PI         Kp 0.45 Ku   Ti Tu/1.2  Td 0
 * A heater-only plant can't pull back an overshoot, hence rule 1. */
#define ZN_RULE              1

/* ---------------------------------------------------------------------------
 * Relay autotune
 * ------------------------------------------------------------------------- */
#define RELAY_HYST_C         0.3f
#define RELAY_AMPLITUDE      50.0f    /* heater swings 0..100 %, so d = 50 */
#define RELAY_SWITCHES       6        /* switches 3..6 are used */
#define AUTOTUNE_TIMEOUT_S   (45u * 60u)
#define AUTOTUNE_FAN_SHIFT_C 8.0f     /* fans only act as a safety net while tuning */

/* ---------------------------------------------------------------------------
 * Fans
 * ------------------------------------------------------------------------- */
#define FAN1_ONSET_C         2.0f     /* degrees above SP */
#define FAN2_ONSET_C         5.0f
#define FAN_HYST_C           1.5f     /* this far below onset before stopping */
#define FAN_MIN_ON_MS        3000u
#define FAN_RAMP_RANGE_C     8.0f     /* min speed at onset, full this far above */
#define FAN_MIN_PCT          31.0f    /* stall threshold (was PWM 80/255) */
#define FAN_SLEW_PCT_PER_S   78.0f
#define FAN1_SCALE           1.00f
#define FAN2_SCALE           1.20f    /* fan 2 needs a bit more drive */

/* ---------------------------------------------------------------------------
 * Step metrics
 * ------------------------------------------------------------------------- */
#define SETTLE_BAND_FRAC     0.02f    /* of the step size */
#define SETTLE_BAND_MIN_C    0.2f     /* never tighter than sensor noise */
#define SETTLE_CONFIRM_S     30.0f
#define MIN_STEP_C           1.0f

/* ---------------------------------------------------------------------------
 * Buzzer
 * ------------------------------------------------------------------------- */
#define BUZZER_DEFAULT_VOLUME 7       /* 0..10, scales note length */

#endif
