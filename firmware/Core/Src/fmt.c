#include "fmt.h"

#include <math.h>
#include <stdio.h>

char *fmt_float(char *buf, size_t size, float value, uint8_t decimals, bool force_sign)
{
  if (size == 0) return buf;

  if (isnan(value)) {
    snprintf(buf, size, "nan");
    return buf;
  }
  if (isinf(value)) {
    snprintf(buf, size, value > 0 ? "inf" : "-inf");
    return buf;
  }

  if (decimals > 6) decimals = 6;
  uint32_t scale = 1;
  for (uint8_t i = 0; i < decimals; i++) scale *= 10;

  bool  negative  = value < 0.0f;
  float magnitude = negative ? -value : value;
  if (magnitude * (float)scale > 4.0e9f) {
    snprintf(buf, size, negative ? "-ovf" : "ovf");
    return buf;
  }

  uint32_t fixed = (uint32_t)(magnitude * (float)scale + 0.5f);
  unsigned long whole = fixed / scale;
  unsigned long frac  = fixed % scale;

  /* "-0.00" looks odd, so the sign goes when the value rounds to zero. */
  const char *sign = (negative && fixed != 0) ? "-" : (force_sign ? "+" : "");

  if (decimals == 0)
    snprintf(buf, size, "%s%lu", sign, whole);
  else
    snprintf(buf, size, "%s%lu.%0*lu", sign, whole, (int)decimals, frac);
  return buf;
}
