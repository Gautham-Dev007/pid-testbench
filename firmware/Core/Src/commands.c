#include "commands.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "console.h"

#define MAX_TOKENS 5

/* Strict number parsing: the whole token must be a finite number. */
static bool parse_float(const char *s, float *out)
{
  if (!s || !*s) return false;
  char *end;
  float v = strtof(s, &end);
  if (*end != '\0' || !isfinite(v)) return false;
  *out = v;
  return true;
}

static bool parse_int(const char *s, int *out)
{
  if (!s || !*s) return false;
  char *end;
  long v = strtol(s, &end, 10);
  if (*end != '\0' || v < -32768 || v > 32767) return false;
  *out = (int)v;
  return true;
}

static bool is(const char *a, const char *b) { return strcmp(a, b) == 0; }

void commands_print_help(void)
{
  console_printf(
    "Commands:\r\n"
    "  START | PI        start control in the current mode\r\n"
    "  STOP | H0         heater off (also clears a fault once it's gone)\r\n"
    "  AUTOTUNE          relay autotune at the current setpoint, then ZN\r\n"
    "  RESET             clear the integrator and restart the step metrics\r\n"
    "  FANMODE [ON|OFF]  fans full, heater locked off (toggles without arg)\r\n"
    "  Sxx.x             setpoint in C, e.g. S75 or S72.5\r\n"
    "  MODE 0|1|2        0 MANUAL, 1 ZN, 2 ADAPTIVE\r\n"
    "  SETGAINS kp ki kd set MANUAL gains (until reboot) and switch to it\r\n"
    "  GAINS | METRICS | STATUS\r\n"
    "  Vn                buzzer volume 0..10\r\n"
    "  FACTORYRESET      forget setpoint, volume and autotune results\r\n");
}

void commands_execute(const char *line)
{
  char buf[64];
  size_t n = 0;
  for (; line[n] && n < sizeof buf - 1; n++) buf[n] = (char)toupper((unsigned char)line[n]);
  buf[n] = '\0';

  char *tok[MAX_TOKENS] = {0};
  int   count = 0;
  for (char *p = strtok(buf, " \t,"); p && count < MAX_TOKENS; p = strtok(NULL, " \t,")) tok[count++] = p;
  if (count == 0) return;

  const char *cmd = tok[0];
  const char *arg = count > 1 ? tok[1] : NULL;
  float f;
  int   i;

  if (is(cmd, "START") || is(cmd, "PI")) app_cmd_start();
  else if (is(cmd, "STOP") || is(cmd, "H0")) app_cmd_stop();
  else if (is(cmd, "AUTOTUNE")) app_cmd_autotune();
  else if (is(cmd, "RESET")) app_cmd_reset();
  else if (is(cmd, "GAINS")) app_cmd_gains();
  else if (is(cmd, "METRICS")) app_cmd_metrics();
  else if (is(cmd, "STATUS")) app_cmd_status();
  else if (is(cmd, "HELP") || is(cmd, "?")) commands_print_help();
  else if (is(cmd, "FACTORYRESET")) app_cmd_factory_reset();
  else if (is(cmd, "FANMODE")) {
    if (!arg) app_cmd_fanmode(-1);
    else if (is(arg, "ON")) app_cmd_fanmode(1);
    else if (is(arg, "OFF")) app_cmd_fanmode(0);
    else console_printf("ERR usage: FANMODE [ON|OFF]\r\n");
  }
  else if (strncmp(cmd, "MODE", 4) == 0) {
    /* Accept both "MODE 2" and "MODE2". */
    const char *m = cmd[4] ? cmd + 4 : arg;
    if (parse_int(m, &i) && i >= MODE_MANUAL && i <= MODE_ADAPTIVE) app_cmd_mode(i);
    else console_printf("ERR usage: MODE 0|1|2\r\n");
  }
  else if (is(cmd, "SETGAINS")) {
    Gains g;
    if (count == 4 && parse_float(tok[1], &g.kp) && parse_float(tok[2], &g.ki) && parse_float(tok[3], &g.kd) &&
        g.kp >= 0 && g.ki >= 0 && g.kd >= 0 && g.kp <= 1000 && g.ki <= 1000 && g.kd <= 1000)
      app_cmd_set_gains(g);
    else
      console_printf("ERR usage: SETGAINS kp ki kd (each 0..1000)\r\n");
  }
  else if (cmd[0] == 'V' && (parse_int(cmd + 1, &i) || (!cmd[1] && parse_int(arg, &i)))) {
    app_cmd_volume(i);
  }
  else if (cmd[0] == 'S' && (parse_float(cmd + 1, &f) || ((!cmd[1] || is(cmd, "SP")) && parse_float(arg, &f)))) {
    app_cmd_setpoint(f);
  }
  else {
    console_printf("ERR unknown command '%s', send HELP\r\n", line);
  }
}
