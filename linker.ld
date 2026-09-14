/* FortressOS Linker Script
 * Target: x86_64 Higher-Half Virtual Addressing (Limine Protocol)
 */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)

ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS((1 << 0) | (1 << 2)); /* Execute + Read */
    rodata PT_LOAD FLAGS((1 << 2));            /* Read-Only */
    data   PT_LOAD FLAGS((1 << 1) | (1 << 2)); /* Read + Write */
}

SECTIONS
{
    /* Kernel loaded at standard higher-half virtual address */
    . = 0xffffffff80000000;

    __kernel_start = .;

    .text : {
        __text_start = .;
        *(.text .text.*)
        . = ALIGN(4096);
        __text_end = .;
    } :text

    . = ALIGN(4096);

    .rodata : {
        __rodata_start = .;
        *(.rodata .rodata.*)
        . = ALIGN(4096);
        __rodata_end = .;
    } :rodata

    . = ALIGN(4096);

    .data : {
        __data_start = .;
        *(.data .data.*)

        /* Limine Bootloader Protocol Requests */
        KEEP(*(.requests_start_marker))
        KEEP(*(.requests))
        KEEP(*(.requests_end_marker))
    } :data

    .bss : {
        *(COMMON)
        *(.bss .bss.*)
        . = ALIGN(4096);
        __kernel_end = .;
    } :data

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
        *(.comment)
    }
}
