#ifndef SHELL_FILEEDIT_H
#define SHELL_FILEEDIT_H

#include "types.h"

void editor_load(const char *path);
bool editor_ready(void);
void editor_loop(void);

#endif /* SHELL_FILEEDIT_H */
