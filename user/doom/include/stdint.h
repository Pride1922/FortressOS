#ifndef DOOM_LIBC_STDINT_H
#define DOOM_LIBC_STDINT_H

typedef signed char        int8_t;
typedef short              int16_t;
typedef int                int32_t;
typedef long long          int64_t;

typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;

typedef long long          intptr_t;
typedef unsigned long long uintptr_t;

typedef long long          intmax_t;
typedef unsigned long long uintmax_t;

#define INT32_MAX 2147483647
#define INT32_MIN (-2147483647 - 1)
#define UINT32_MAX 4294967295U

#define INT64_MAX 9223372036854775807LL
#define INT64_MIN (-9223372036854775807LL - 1)
#define UINT64_MAX 18446744073709551615ULL

#endif
