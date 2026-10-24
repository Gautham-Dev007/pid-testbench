#ifndef FMT_H
#define FMT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* newlib-nano is linked without float printf by default, so "%f" prints
 * nothing. Floats are formatted with this instead. Returns buf. */
char *fmt_float(char *buf, size_t size, float value, uint8_t decimals, bool force_sign);

#endif
