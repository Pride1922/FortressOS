[bits 64]
default rel

section .rodata
global embedded_init_elf_start
global embedded_init_elf_end

embedded_init_elf_start:
    incbin "build/init.elf"
embedded_init_elf_end:
