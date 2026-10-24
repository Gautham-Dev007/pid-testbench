#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdbool.h>
#include <stddef.h>

/* Interrupt-driven USART2 (ST-LINK virtual COM port). Writes never block: if
 * the host stops reading, output is dropped rather than stalling control. */

void console_init(void);
void console_write(const char *data, size_t len);
void console_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Copies one complete line (without CR/LF) into buf. Returns false if no
 * full line has arrived yet. Over-long lines are discarded. */
bool console_read_line(char *buf, size_t size);

#endif
