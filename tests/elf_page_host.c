#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "elf_page.h"

int main(void) {
    uint8_t storage[4096 + 64], source[4096 + 32], expected[4096];
    for (size_t i = 0; i < sizeof(source); i++) source[i] = (uint8_t)(i * 37 + 13);
    for (size_t offset = 0; offset <= 4096; offset++) {
        size_t lengths[] = {0, 4096-offset, (4096-offset)/2, offset < 4096 ? 1 : 0};
        for (size_t c = 0; c < 4; c++) {
            size_t length = lengths[c];
            memset(storage, 0xa5, sizeof(storage));
            memset(expected, 0, sizeof(expected));
            memcpy(expected + offset, source + 3, length);
            elf_page_init(storage + 17, source + 3, offset, length);
            assert(memcmp(storage + 17, expected, 4096) == 0);
            for (size_t i = 0; i < 17; i++) assert(storage[i] == 0xa5);
            for (size_t i = 4113; i < sizeof(storage); i++) assert(storage[i] == 0xa5);
        }
    }
    puts("ELF page initialization: 16388 offset/length/canary cases PASS");
}
