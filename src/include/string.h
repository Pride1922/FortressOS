#ifndef FORTRESS_STRING_H
#define FORTRESS_STRING_H

#include "types.h"

void *memset(void *dest, int ch, size_t count);
void *memcpy(void *dest, const void *src, size_t count);
void *memmove(void *dest, const void *src, size_t count);
int   memcmp(const void *lhs, const void *rhs, size_t count);
size_t strlen(const char *str);

#endif /* FORTRESS_STRING_H */
