/* BEX2 v1: 4 MiB of segment offsets. The header/null page is never mapped. */
ENTRY(_start)
OUTPUT_FORMAT(elf32-i386)
PHDRS { text PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); }
__workspace_bytes = DEFINED(__bex2_workspace_bytes) ? __bex2_workspace_bytes : 1048576;
__stack_bytes = DEFINED(__bex2_stack_bytes) ? __bex2_stack_bytes : 65536;
__required_abi_minor = DEFINED(__bex2_required_abi_minor) ? __bex2_required_abi_minor : 1;
SECTIONS {
    . = 0;
    .header : {
        LONG(0x32584542) LONG(64) LONG(1) LONG(0)
        LONG(__file_end) LONG(_start) LONG(__text_end - __text_start) LONG(__data_start)
        LONG(__data_file_end - __data_start) LONG(__data_memory_end - __data_start)
        LONG(__workspace_bytes) LONG(__stack_bytes) LONG(1) LONG(__required_abi_minor)
        LONG(0) LONG(0)
    } :text
    . = 4096;
    __text_start = .;
    .text : { *(.text.start) *(.text .text.*) } :text
    .rodata : { *(.rodata .rodata.*) } :text
    __text_end = .;
    . = ALIGN(4096);
    __data_start = .;
    .data : { *(.data .data.*) } :data
    __data_file_end = .;
    __file_end = .;
    .bss (NOLOAD) : { *(.bss .bss.*) *(COMMON) . = ALIGN(16); } :data
    __data_memory_end = .;
    __workspace_start = ALIGN(__data_memory_end, 4096);
    __workspace_end = __workspace_start + __workspace_bytes;
    __stack_top = 4194304;
    __stack_bottom = __stack_top - __stack_bytes;
    __stack_guard = __stack_bottom - 4096;
    /* Explicitly unsupported runtime facilities must fail instead of becoming
     * orphan loadable sections with header-invisible file/memory footprints. */
    .unsupported : {
        *(.tdata .tdata.* .tbss .tbss.*) *(.got .got.* .plt .plt.* .iplt)
        *(.dynamic .dynsym .dynstr .interp .hash .gnu.hash)
        *(.rel .rel.* .rela .rela.*)
        *(.init .fini .init_array .init_array.* .fini_array .fini_array.*)
        *(.preinit_array .preinit_array.* .ctors .ctors.* .dtors .dtors.*)
    } :data
    ASSERT(SIZEOF(.header) == 64, "BEX2 header must be 64 bytes")
    ASSERT(__text_end > __text_start, "BEX2 requires nonempty text")
    ASSERT(_start >= __text_start && _start < __text_end, "BEX2 entry must be in text")
    ASSERT(__file_end <= 262144, "BEX2 executable exceeds 256 KiB file limit")
    ASSERT((__workspace_bytes % 4096) == 0, "BEX2 workspace must be a page multiple")
    ASSERT(__stack_bytes >= 16384 && __stack_bytes <= 262144 && (__stack_bytes % 4096) == 0,
           "BEX2 stack must be a page multiple between 16 and 256 KiB")
    ASSERT(__workspace_end <= __stack_guard, "BEX2 data/workspace overlaps stack guard")
    ASSERT(SIZEOF(.unsupported) == 0, "BEX2 does not support TLS, dynamic linking or constructors")
    /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) }
}
