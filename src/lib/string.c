#include "string.h"

/* Prevent GCC from transforming loops in memset/memcpy into calls to themselves */
__attribute__((optimize("no-tree-loop-distribute-patterns")))
void *memset(void *dest, int ch, size_t count) {
    uint8_t *d = (uint8_t *)dest;
    uint8_t  val = (uint8_t)ch;

    for (size_t i = 0; i < count; i++) {
        d[i] = val;
    }
    return dest;
}

__attribute__((optimize("no-tree-loop-distribute-patterns")))
void *memcpy(void *dest, const void *src, size_t count) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;

    for (size_t i = 0; i < count; i++) {
        d[i] = s[i];
    }
    return dest;
}

__attribute__((optimize("no-tree-loop-distribute-patterns")))
void *memmove(void *dest, const void *src, size_t count) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;

    if (d == s || count == 0) {
        return dest;
    }

    if (d < s) {
        /* Copy forward */
        for (size_t i = 0; i < count; i++) {
            d[i] = s[i];
        }
    } else {
        /* Copy backward to handle overlapping buffers */
        for (size_t i = count; i > 0; i--) {
            d[i - 1] = s[i - 1];
        }
    }
    return dest;
}

int memcmp(const void *lhs, const void *rhs, size_t count) {
    const uint8_t *a = (const uint8_t *)lhs;
    const uint8_t *b = (const uint8_t *)rhs;

    for (size_t i = 0; i < count; i++) {
        if (a[i] < b[i]) return -1;
        if (a[i] > b[i]) return 1;
    }
    return 0;
}

size_t strlen(const char *str) {
    if (!str) return 0;
    size_t len = 0;
    while (str[len] != '\0') {
        len++;
    }
    return len;
}

int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s1[i] != s2[i] || s1[i] == '\0') {
            return (unsigned char)s1[i] - (unsigned char)s2[i];
        }
    }
    return 0;
}
