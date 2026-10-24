#include "display.h"

#include <stdio.h>
#include <string.h>

#include "fmt.h"
#include "main.h"
#include "sh1106.h"

/* Layout, 128 x 64:
 *   y  0..9   header bar
 *   y 11..53  page content
 *   y 54      footer rule
 *   y 56..63  footer: heater dot, page dots, mode tag */

#define DEG "\x7f"

static uint8_t  page;
static uint32_t page_since;
static uint8_t  frame;

static void bar(int x, int y, int w, int h, float pct)
{
  sh1106_frame(x, y, w, h, INK_ON);
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  int fill = (int)((float)(w - 2) * pct / 100.0f + 0.5f);
  if (fill > 0) sh1106_box(x + 1, y + 1, fill, h - 2, INK_ON);
}

static void text_right(int right, int y, const char *s, int scale, Ink ink)
{
  sh1106_text(right - sh1106_text_width(s, scale), y, s, scale, ink);
}

static void header(void)
{
  sh1106_box(0, 0, OLED_W, 10, INK_ON);
  sh1106_text(2, 1, "IEEE SPS x WIE", 1, INK_OFF);
  text_right(OLED_W - 2, 1, "PID", 1, INK_OFF);
}

static void footer(const DisplayData *d, bool blink)
{
  sh1106_hline(0, 54, OLED_W, INK_ON);

  if (d->heater_pct > 0.5f && blink) sh1106_disc(4, 59, 3, INK_ON);
  else sh1106_circle(4, 59, 3, INK_ON);
  sh1106_text(10, 56, d->heater_pct > 0.5f ? "HT" : "--", 1, INK_ON);

  for (int i = 0; i < 3; i++) {
    if (i == page) sh1106_disc(57 + i * 7, 59, 2, INK_ON);
    else sh1106_circle(57 + i * 7, 59, 2, INK_ON);
  }

  int w = sh1106_text_width(d->mode_tag, 1);
  if (!d->running || d->fan_mode) {
    sh1106_box(OLED_W - w - 4, 55, w + 4, 9, INK_ON);
    sh1106_text(OLED_W - w - 2, 56, d->mode_tag, 1, INK_OFF);
  } else {
    sh1106_text(OLED_W - w - 1, 56, d->mode_tag, 1, INK_ON);
  }
}

static void page_main(const DisplayData *d, bool blink)
{
  char buf[16], line[24];

  if (d->temp_valid) fmt_float(buf, sizeof buf, d->temp, 1, false);
  else strcpy(buf, "---.-");
  int x = sh1106_text(0, 13, buf, 2, INK_ON);
  sh1106_text(x, 13, DEG "C", 1, INK_ON);
  if (d->overshoot && !blink) sh1106_box(0, 12, 78, 16, INK_XOR);

  sh1106_vline(80, 11, 17, INK_ON);
  sh1106_text(84, 12, "SP", 1, INK_ON);
  fmt_float(buf, sizeof buf, d->setpoint, 1, false);
  snprintf(line, sizeof line, "%s" DEG, buf);
  sh1106_text(84, 21, line, 1, INK_ON);

  sh1106_hline(0, 29, OLED_W, INK_ON);

  bar(0, 31, OLED_W, 11, d->heater_pct);
  snprintf(line, sizeof line, "HTR %3d%%", (int)(d->heater_pct + 0.5f));
  sh1106_text(3, 33, line, 1, INK_XOR);

  float err = d->setpoint - d->temp;
  if (d->temp_valid) {
    fmt_float(buf, sizeof buf, err, 1, true);
    snprintf(line, sizeof line, "E%s", buf);
    sh1106_text(0, 45, line, 1, INK_ON);
    fmt_float(buf, sizeof buf, d->dtdt, 2, true);
    snprintf(line, sizeof line, "%sC/s", buf);
    text_right(OLED_W, 45, line, 1, INK_ON);
  }
}

static void page_fans(const DisplayData *d)
{
  static const char blades[] = "|/-\\";
  char line[16];
  uint8_t pct[2] = {d->fan1_pct, d->fan2_pct};

  for (int i = 0; i < 2; i++) {
    int y = 12 + i * 18;
    snprintf(line, sizeof line, "FAN%d", i + 1);
    sh1106_text(0, y, line, 1, INK_ON);
    if (pct[i] > 0) {
      char b[2] = {blades[((frame >> 1) + i * 2) & 3], 0};
      sh1106_text(30, y, b, 1, INK_ON);
    }
    snprintf(line, sizeof line, "%3d%%", pct[i]);
    text_right(OLED_W, y, line, 1, INK_ON);
    bar(0, y + 8, OLED_W, 8, pct[i]);
  }

  const char *status = d->fan_mode ? "FANMODE: manual"
                     : (d->fan1_pct || d->fan2_pct) ? "ACTIVE: cooling" : "STANDBY";
  sh1106_text(0, 47, status, 1, INK_ON);
}

static void page_gains(const DisplayData *d, bool blink)
{
  static const char *zone_names[] = {"FAR", "MID", "NEAR"};
  char buf[16], line[24];

  const struct { const char *name; float v; uint8_t dec; } rows[] = {
    {"Kp", d->gains.kp, 3}, {"Ki", d->gains.ki, 4}, {"Kd", d->gains.kd, 3},
  };
  for (int i = 0; i < 3; i++) {
    sh1106_text(0, 12 + i * 9, rows[i].name, 1, INK_ON);
    sh1106_text(16, 12 + i * 9, fmt_float(buf, sizeof buf, rows[i].v, rows[i].dec, false), 1, INK_ON);
  }

  const char *source = d->zone >= 0 && d->zone < 3 ? zone_names[d->zone]
                     : d->tuned ? "tuned" : "default";
  text_right(OLED_W, 12, source, 1, INK_ON);

  sh1106_hline(0, 38, OLED_W, INK_ON);

  if (d->autotuning) {
    bar(0, 40, OLED_W, 6, 100.0f * d->tune_switches / RELAY_SWITCHES);
    if (blink) snprintf(line, sizeof line, "TUNE %d/%d switches", d->tune_switches, RELAY_SWITCHES);
    else snprintf(line, sizeof line, "TUNING...");
    sh1106_text(0, 47, line, 1, INK_ON);
  } else if (d->temp_valid) {
    fmt_float(buf, sizeof buf, d->dtdt, 2, true);
    snprintf(line, sizeof line, "%sC/s", buf);
    sh1106_text(0, 42, line, 1, INK_ON);
    fmt_float(buf, sizeof buf, d->setpoint - d->temp, 1, true);
    snprintf(line, sizeof line, "E%s", buf);
    text_right(OLED_W, 42, line, 1, INK_ON);
  }
}

static void fault_screen(const DisplayData *d, bool blink)
{
  header();
  if (blink) sh1106_text((OLED_W - sh1106_text_width("FAULT", 2)) / 2, 14, "FAULT", 2, INK_ON);
  sh1106_text(0, 33, d->fault_title, 1, INK_ON);
  sh1106_text(0, 43, d->fault_detail, 1, INK_ON);
  sh1106_text(0, 55, "Heater off", 1, INK_ON);
}

void display_init(void)
{
  sh1106_init();

  for (int step = 0; step <= 10; step++) {
    sh1106_clear();
    header();
    sh1106_text((OLED_W - sh1106_text_width("PID CTRL", 2)) / 2, 18, "PID CTRL", 2, INK_ON);
    sh1106_text((OLED_W - sh1106_text_width("STM32 G070RB", 1)) / 2, 38, "STM32 G070RB", 1, INK_ON);
    bar(0, 54, OLED_W, 8, step * 10.0f);
    sh1106_flush_all();
    HAL_Delay(40);
  }
  page_since = HAL_GetTick();
}

void display_render(const DisplayData *d, uint32_t now)
{
  frame = (uint8_t)((frame + 1u) % 10u);
  bool blink = frame < 5;  /* about 1 Hz at a 5 Hz redraw */

  sh1106_clear();

  if (d->fault) {
    fault_screen(d, blink);
    sh1106_present();
    return;
  }

  if (now - page_since >= OLED_PAGE_MS) {
    page_since = now;
    page = (uint8_t)((page + 1u) % 3u);
    /* Keep the cooling page up while the fans run, and the main page while
     * stopped, so the useful screen is the one showing. */
    if ((d->fan1_pct || d->fan2_pct) && page == 2) page = 0;
    if (!d->running) page = 0;
  }

  header();
  switch (page) {
    case 0: page_main(d, blink); break;
    case 1: page_fans(d); break;
    default: page_gains(d, blink); break;
  }
  footer(d, blink);

  if (d->overshoot && blink) {
    sh1106_frame(0, 0, OLED_W, OLED_H, INK_ON);
    sh1106_frame(2, 2, OLED_W - 4, OLED_H - 4, INK_ON);
  }
  sh1106_present();
}

void display_service(void) { sh1106_flush_step(); }
