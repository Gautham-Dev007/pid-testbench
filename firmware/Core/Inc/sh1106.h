#ifndef SH1106_H
#define SH1106_H

#include <stdbool.h>
#include <stdint.h>

/* 128x64 SH1106 over I2C1 with a local frame buffer. Drawing only touches
 * RAM; sh1106_flush_step() sends one 128-byte page per call so a full frame
 * never holds up the control loop for more than ~3 ms at a time. */

#define OLED_W 128
#define OLED_H 64

typedef enum { INK_OFF = 0, INK_ON = 1, INK_XOR = 2 } Ink;

bool sh1106_init(void);
void sh1106_clear(void);
void sh1106_pixel(int x, int y, Ink ink);
void sh1106_hline(int x, int y, int w, Ink ink);
void sh1106_vline(int x, int y, int h, Ink ink);
void sh1106_frame(int x, int y, int w, int h, Ink ink);
void sh1106_box(int x, int y, int w, int h, Ink ink);
void sh1106_disc(int cx, int cy, int r, Ink ink);
void sh1106_circle(int cx, int cy, int r, Ink ink);

/* 5x7 font on a 6x8 cell, scaled by an integer. Returns the x after the text. */
int sh1106_text(int x, int y, const char *s, int scale, Ink ink);
int sh1106_text_width(const char *s, int scale);

/* Marks the buffer for sending. */
void sh1106_present(void);
/* Sends the next pending page, if any. Returns true while a frame is still going out. */
bool sh1106_flush_step(void);
/* Sends everything now (start-up and fault screens only). */
void sh1106_flush_all(void);

#endif
