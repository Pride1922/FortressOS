/* Same load permissions as user/shell.ld, with a final dedicated writable
 * page for partial-range tests. The following page is deliberately unmapped. */
ENTRY(_start)
PHDRS { text PT_LOAD FLAGS(5); rodata PT_LOAD FLAGS(4); data PT_LOAD FLAGS(6); }
SECTIONS {
    . = 0x400000;
    .text : { *(.text .text.*) } :text
    . = ALIGN(0x1000);
    .rodata : { *(.rodata .rodata.*) } :rodata
    . = ALIGN(0x1000);
    .data : { *(.data .data.*) } :data
    .bss : { *(.bss .bss.*) *(COMMON) . = ALIGN(0x1000); . += 0x1000; probe_data_end = .; } :data
    /DISCARD/ : { *(.eh_frame*) *(.comment) *(.note*) }
}
