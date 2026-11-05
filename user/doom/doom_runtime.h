#ifndef FORTRESS_DOOM_RUNTIME_H
#define FORTRESS_DOOM_RUNTIME_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Runtime configuration & stats */
typedef struct {
    uint64_t heap_arena_base;
    size_t heap_arena_size;
    size_t heap_allocated_current;
    size_t heap_allocated_peak;
    size_t heap_alloc_count;
    size_t heap_free_count;
    size_t file_open_count;
    size_t file_read_bytes;
    size_t file_seek_count;
    size_t file_backward_seek_count;
} doom_runtime_stats_t;

void doom_runtime_init(void);
void doom_runtime_get_stats(doom_runtime_stats_t *out);

/* Syscall invoker for freestanding FortressOS */
long doom_syscall(long num, uintptr_t a1, uintptr_t a2, uintptr_t a3,
                  uintptr_t a4, uintptr_t a5, uintptr_t a6);

#ifdef __cplusplus
}
#endif

#endif /* FORTRESS_DOOM_RUNTIME_H */
