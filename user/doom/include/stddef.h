#ifndef DOOM_LIBC_STDDEF_H
#define DOOM_LIBC_STDDEF_H

typedef unsigned long long size_t;
typedef long long ptrdiff_t;

#define NULL ((void *)0)
#define offsetof(type, member) __builtin_offsetof(type, member)
#define SIZE_MAX 18446744073709551615ULL

#endif
