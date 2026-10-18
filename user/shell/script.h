#ifndef SHELL_SCRIPT_H
#define SHELL_SCRIPT_H
#include "types.h"
/* eval returns -1 for incomplete input, -2 for fatal syntax, or a command status. */
int script_run(int fd,const char *text,const char *name,int (*eval)(const char *));
extern volatile unsigned script_interrupted;
extern bool shell_script_mode;
#endif
