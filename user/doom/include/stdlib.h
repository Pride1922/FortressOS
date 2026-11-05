#ifndef DOOM_LIBC_STDLIB_H
#define DOOM_LIBC_STDLIB_H

#include <stddef.h>

void *malloc(size_t size);
void free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);

void exit(int status) __attribute__((noreturn));
int atoi(const char *nptr);
double atof(const char *nptr);
int abs(int j);
int system(const char *command);
char *getenv(const char *name);

#endif
