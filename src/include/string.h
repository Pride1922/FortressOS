#ifndef FORTRESS_STRING_H
#define FORTRESS_STRING_H

#include "types.h"

void *memset(void *dest, int ch, size_t count);
void *memcpy(void *dest, const void *src, size_t count);
void *memmove(void *dest, const void *src, size_t count);
int   memcmp(const void *lhs, const void *rhs, size_t count);
size_t strlen(const char *str);
int   strcmp(const char *s1, const char *s2);
int   strncmp(const char *s1, const char *s2, size_t n);

#endif /* FORTRESS_STRING_H */
