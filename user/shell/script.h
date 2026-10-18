#ifndef SHELL_SCRIPT_H
#define SHELL_SCRIPT_H
#include "types.h"
#include "control.h"
typedef struct {
    char unit[CONTROL_SOURCE_CAP];
    size_t used,line,start;
    bool cr,eof;
} script_reader_t;
void script_reader_init(script_reader_t *reader);
/* 1=compiled unit, 0=EOF, -2=read/parse/budget failure. */
int script_reader_next(script_reader_t *reader,int fd,const char *name,
                       int (*compile)(const char *));
/* eval returns -1 for incomplete input, -2 for fatal syntax, or a command status. */
int script_run(int fd,const char *text,const char *name,int (*eval)(const char *));
extern volatile unsigned script_interrupted;
extern bool shell_script_mode;
#endif
