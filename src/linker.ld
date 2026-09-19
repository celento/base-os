#include "layout.h"
ENTRY(_start)
OUTPUT_FORMAT(elf32-i386)
PHDRS {
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
}
SECTIONS {
    . = KERNEL_LOAD_ADDR;
    .text : { KEEP(*(.text.entry)) *(.text .text.*) } :text
    .rodata : { *(.rodata .rodata.*) } :text
    . = ALIGN(16);
    .data : { *(.data .data.*) } :data
    __load_end = .;
    .bss (NOLOAD) : {
        . = ALIGN(16);
        __bss_start = .;
        *(.bss .bss.*) *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    } :data
    __kernel_end = .;
    ASSERT(_start == KERNEL_LOAD_ADDR, "entry must be first in image")
    ASSERT(__load_end - KERNEL_LOAD_ADDR <= KERNEL_SECTORS * SECTOR_SIZE,
           "kernel exceeds boot loader reservation")
    ASSERT(__kernel_end <= STACK_BOTTOM, "kernel overlaps reserved stack")
    ASSERT(1 + KERNEL_SECTORS <= FS_DISK_LBA, "kernel overlaps filesystem")
    /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) }
}
