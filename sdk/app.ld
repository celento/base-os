ENTRY(_start)
OUTPUT_FORMAT(elf32-i386)
PHDRS { text PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); }
SECTIONS {
    . = 0;
    .header : { LONG(0x31584542) LONG(_start) LONG(__file_end) LONG(0) } :text
    .text : { *(.text.start) *(.text .text.*) } :text
    .rodata : { *(.rodata .rodata.*) } :text
    .data : { *(.data .data.*) } :data
    __file_end = .;
    .bss (NOLOAD) : { *(.bss .bss.*) *(COMMON) } :data
    ASSERT(__file_end <= 16383, "BEX1 executable exceeds 16383-byte image limit")
    ASSERT(. <= 49152, "application needs to leave 16 KiB for its stack")
    /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) }
}
