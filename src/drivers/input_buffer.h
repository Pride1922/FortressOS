#ifndef FORTRESS_INPUT_BUFFER_H
#define FORTRESS_INPUT_BUFFER_H
#include "types.h"
#define INPUT_CAPACITY 256
/* Caller serializes access. Full queues drop the newest byte, preserving FIFO. */
typedef struct {
    char bytes[INPUT_CAPACITY];
    size_t head, count;
    uint64_t dropped;
} input_buffer_t;
static inline bool input_buffer_push(input_buffer_t *q, char c) {
    if (q->count == INPUT_CAPACITY) { __atomic_fetch_add(&q->dropped, 1, __ATOMIC_RELAXED); return false; }
    q->bytes[(q->head + q->count) % INPUT_CAPACITY] = c;
    q->count++;
    return true;
}
static inline size_t input_buffer_read(input_buffer_t *q, char *out, size_t count) {
    size_t n = 0;
    while (n < count && q->count) {
        out[n++] = q->bytes[q->head];
        q->head = (q->head + 1) % INPUT_CAPACITY;
        q->count--;
    }
    return n;
}
static inline bool input_buffer_sequence(input_buffer_t *q, const char *s, size_t n) {
    if (n > INPUT_CAPACITY - q->count) { __atomic_fetch_add(&q->dropped, n, __ATOMIC_RELAXED); return false; }
    for (size_t i = 0; i < n; i++) input_buffer_push(q, s[i]);
    return true;
}
#endif
