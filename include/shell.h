#ifndef ASAS_SHELL_H
#define ASAS_SHELL_H

int shell_execute(const char *command);
int shell_self_test(void);
void shell_poll_input_once(void);
int shell_start_interactive(void);

#endif
