#ifndef KERNEL_DMESG_H
#define KERNEL_DMESG_H

#include "types.h"
#include "syscall.h"   /* for DMESG_SIZE */

void dmesg_append(char c);
void dmesg_append_str(const char *s, size_t n);
size_t dmesg_read(char *dst, size_t cap);

#endif