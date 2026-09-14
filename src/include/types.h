#ifndef FORTRESS_TYPES_H
#define FORTRESS_TYPES_H

/* FortressOS Freestanding Primitive Types
 *
 * In conforming freestanding C11 implementations (ISO/IEC 9899:2011 §4/6),
 * <stdint.h>, <stddef.h>, and <stdbool.h> are compiler-provided runtime-free headers.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef int64_t  ssize_t;
typedef int64_t  ptrdiff_t;

#ifndef NULL
#define NULL ((void *)0)
#endif

/* Alignment and utility macros */
#define ALIGN_UP(x, align)   (((x) + ((align) - 1)) & ~((align) - 1))
#define ALIGN_DOWN(x, align) ((x) & ~((align) - 1))
#define DIV_ROUND_UP(n, d)   (((n) + (d) - 1) / (d))

#endif /* FORTRESS_TYPES_H */
