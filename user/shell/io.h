#ifndef SHELL_IO_H
#define SHELL_IO_H
#include "types.h"
#include "syscall_abi.h"
#include "vfs.h"
#define WRITE_CHUNK 4096
long call(long nr, uintptr_t a, uintptr_t b, uintptr_t c);
size_t length(const char *s);
bool equal(const char *a, const char *b);
#if defined(__STDC_HOSTED__) && __STDC_HOSTED__ == 1
int puts(const char *s);
#else
void puts(const char *s);
#endif
void put_dec(size_t val);
void write_bytes(const char *s, size_t n);
void file_error(long error);
void editor_load(const char *path);
void editor_loop(void);
bool editor_ready(void);
#endif
