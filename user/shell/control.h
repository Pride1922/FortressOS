#ifndef SHELL_CONTROL_H
#define SHELL_CONTROL_H
#include "types.h"
#define CONTROL_SOURCE_CAP 16385
#define CONTROL_MAX_SOURCES 8
#define CONTROL_YIELD (-3)
#define CONTROL_SOURCE_PENDING 2u
#define CONTROL_RETURN_PENDING 3u
/* Select a stable BSS execution slot; suspended slots retain their PC/loops. */
bool control_select(unsigned index);
#define CONTROL_MAX_NODES 256
#define CONTROL_MAX_TOKENS 1024
#define CONTROL_MAX_DEPTH 8
/* Compile a complete unit before side effects. 0=ready, -1=incomplete,
 * -2=invalid/over budget. Storage is owned BSS, not parser scratch. */
int control_compile(const char *source);
const char *control_error(void);
bool control_empty(void);
int control_execute(int status,int (*simple)(const char *,int),volatile unsigned *interrupted);
/* Shared by break/continue and the command-list executor. */
extern unsigned control_stop;
int control_request(bool again,int argc,char **argv);
#endif
