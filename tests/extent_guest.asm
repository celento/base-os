; Place reachable code beyond the old 196096-byte disk reservation.
bits 32
section .text
global extent_guest
extern kmain, platform_log
extent_guest:
    jmp beyond_primary
    times 135000 db 0x90
beyond_primary:
    push message
    call platform_log
    add esp, 4
    call kmain
section .rodata
message db 'KERNEL-EXTENT-PASS', 10, 0
