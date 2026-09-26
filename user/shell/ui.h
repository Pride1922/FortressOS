#ifndef SHELL_UI_H
#define SHELL_UI_H

#include "lineedit.h"

bool shell_read_line(char out[LINE_CAP], bool continuation);
void shell_history(const char *arg);
void shell_terminal(const char *arg);
void shell_set_prompt_state(int64_t status, const char *cwd);
void shell_set_prompt_template(const char *tmpl);
const char *shell_get_prompt_template(void);
void shell_set_terminal_fd(int fd);
int  shell_get_terminal_fd(void);
void shell_ui_init(void);

#endif /* SHELL_UI_H */
