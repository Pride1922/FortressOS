#ifndef FORTRESS_ELF_PAGE_H
#define FORTRESS_ELF_PAGE_H
#include "types.h"

/* Initialize an exclusively owned, unmapped 4 KiB frame. The loader has
 * bounded offset/length to this page and validated the source slice. Copy
 * each file byte once and zero every byte outside the slice. Integer string
 * instructions require no SIMD state and explicitly establish forward DF. */
static inline void elf_page_init(uint8_t *page, const uint8_t *source,
                                 size_t offset, size_t length) {
    uint8_t *dest = page;
    size_t count = offset;
    __asm__ volatile("cld; rep stosb" : "+D"(dest), "+c"(count) : "a"(0) : "memory", "cc");
    count = length;
    __asm__ volatile("rep movsb" : "+D"(dest), "+S"(source), "+c"(count) : : "memory");
    count = 4096 - offset - length;
    __asm__ volatile("rep stosb" : "+D"(dest), "+c"(count) : "a"(0) : "memory");
}
#endif
