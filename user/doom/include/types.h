#ifndef FORTRESS_TYPES_H
#define FORTRESS_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef DOOM_HOST_TEST
#include <sys/types.h>
#else
typedef int64_t ssize_t;
typedef int64_t ptrdiff_t;
#endif

#ifndef NULL
#define NULL ((void *)0)
#endif

#define ALIGN_UP(x, align)   (((x) + ((align) - 1)) & ~((align) - 1))
#define ALIGN_DOWN(x, align) ((x) & ~((align) - 1))
#define DIV_ROUND_UP(n, d)   (((n) + (d) - 1) / (d))

#endif /* FORTRESS_TYPES_H */
