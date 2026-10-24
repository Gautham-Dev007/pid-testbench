#ifndef APP_H
#define APP_H

#include <stdbool.h>

#include "controller.h"

/* Called from main.c */
void app_init(void);
void app_loop(void);

/* Command handlers, called by commands.c. Each prints its own reply. */
void app_cmd_start(void);
void app_cmd_stop(void);
void app_cmd_autotune(void);
void app_cmd_fanmode(int on);  /* 1 on, 0 off, -1 toggle */
void app_cmd_reset(void);
void app_cmd_setpoint(float value);
void app_cmd_mode(int mode);
void app_cmd_set_gains(Gains g);
void app_cmd_gains(void);
void app_cmd_metrics(void);
void app_cmd_status(void);
void app_cmd_volume(int volume);
void app_cmd_factory_reset(void);

#endif
