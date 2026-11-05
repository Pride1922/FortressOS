#ifndef DOOM_LIBC_ASSERT_H
#define DOOM_LIBC_ASSERT_H

#ifdef NDEBUG
#define assert(expr) ((void)0)
#else
void __doom_assert_fail(const char *expr, const char *file, int line);
#define assert(expr) ((expr) ? (void)0 : __doom_assert_fail(#expr, __FILE__, __LINE__))
#endif

#endif
