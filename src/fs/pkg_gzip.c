#include "pkg_engine_config.h"
#if FORTRESS_PKG_ENGINE
#include "types.h"
#include "string.h"
#define FORTRESS_KERNEL_BUILD 1
long tool_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    (void)nr; (void)a; (void)b; (void)c; return -1;
}
#include "../../user/tools/gzip.h"
#include "../../user/tools/gzip.c"
#endif
