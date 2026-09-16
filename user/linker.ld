OUTPUT_FORMAT("elf64-x86-64")
ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS(5); /* PF_R | PF_X */
    rodata PT_LOAD FLAGS(4); /* PF_R */
    data   PT_LOAD FLAGS(6); /* PF_R | PF_W */
}

SECTIONS
{
    . = 0x400000;

    .text : ALIGN(0x1000) {
        *(.text .text.*)
    } :text

    . = 0x401000;

    .rodata : ALIGN(0x1000) {
        *(.rodata .rodata.*)
    } :rodata

    . = 0x402000;

    .data : ALIGN(0x1000) {
        *(.data .data.*)
    } :data

    . = 0x403000;

    .bss : ALIGN(0x1000) {
        *(.bss .bss.*)
        *(COMMON)
    } :data
}
