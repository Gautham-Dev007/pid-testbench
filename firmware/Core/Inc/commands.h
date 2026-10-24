#ifndef COMMANDS_H
#define COMMANDS_H

/* Parses one line from the serial console and runs it. Case-insensitive. */
void commands_execute(const char *line);
void commands_print_help(void);

#endif
