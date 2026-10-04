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
    ASSERT(__bootstrap_end - _start <= 0x10000,
           "real-mode bootstrap exceeds its code segment")
    ASSERT(KERNEL_STAGE_ADDR % 16 == 0 && KERNEL_LOAD_ADDR % 16 == 0,
           "kernel addresses must be paragraph aligned")
    ASSERT(KERNEL_STAGE_ADDR + KERNEL_SECTORS * SECTOR_SIZE <= KERNEL_STAGE_LIMIT,
           "kernel exceeds conventional staging reservation")
    ASSERT(KERNEL_STAGE_LIMIT <= 0xA0000 && KERNEL_STAGE_LIMIT <= KERNEL_LOAD_ADDR,
           "kernel staging overlaps firmware or relocated kernel")
    ASSERT(KERNEL_LOAD_ADDR >= 0x100000,
           "relocated kernel must use extended RAM")
    ASSERT(STACK_TOP - STACK_BOTTOM == 0x10000 && STACK_TOP <= FB_BASE,
           "kernel stack must fit below framebuffer")
    ASSERT(__load_end - KERNEL_LOAD_ADDR <= KERNEL_SECTORS * SECTOR_SIZE,
           "kernel exceeds boot loader reservation")
    ASSERT(__kernel_end <= STACK_BOTTOM, "kernel overlaps reserved stack")
    ASSERT(1 + KERNEL_PRIMARY_SECTORS <= FS_DISK_LBA, "kernel overlaps filesystem")
    ASSERT(KERNEL_EXT_LBA >= FS_SECOND_LBA + FS_DISK_SECTORS,
           "kernel extension overlaps filesystem")
    ASSERT(KERNEL_EXT_LBA + KERNEL_SECTORS - KERNEL_PRIMARY_SECTORS <= DISK_SECTORS,
           "kernel extension exceeds disk")
    /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) }
}
