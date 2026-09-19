; CHS loader for drive A, 80 cylinders, two heads, 18 or 36 sectors/track.
[org 0x7c00]
bits 16
%if KERNEL_SECTORS + 1 > FS_DISK_LBA
%error "kernel overlaps filesystem"
%endif
start:
    cli
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    mov [boot_drive], dl
    test dl, dl
    jnz disk_error             ; Only drive A is supported by the kernel.
    mov ah, 8
    int 0x13
    jc disk_error
    and cl, 0x3f
    cmp dh, 1
    jne disk_error
    cmp ch, 79
    jne disk_error
    cmp cl, 18
    je .geometry
    cmp cl, 36
    jne disk_error
.geometry:
    mov [spt], cl
    mov ax, KERNEL_LOAD_ADDR >> 4
    mov es, ax
    xor bx, bx
    mov cx, 2
    xor dh, dh
    mov di, KERNEL_SECTORS
.read:
    mov byte [retries], 3
.retry:
    pusha
    push es
    mov ax, 0x0201
    mov dl, [boot_drive]
    int 0x13
    pop es
    popa
    jnc .done
    pusha
    xor ax, ax
    mov dl, [boot_drive]
    int 0x13
    popa
    dec byte [retries]
    jnz .retry
    jmp disk_error
.done:
    dec di
    jz kernel_loaded
    add bx, 512
    jnc .advchs
    mov ax, es
    add ax, 0x1000
    mov es, ax
.advchs:
    inc cl
    cmp cl, [spt]
    jbe .read
    mov cl, 1
    xor dh, 1
    jnz .read
    inc ch
    jmp .read
kernel_loaded:
    cli
    mov dl, [boot_drive]
    mov dh, [spt]
    jmp (KERNEL_LOAD_ADDR >> 4):0

disk_error:
    mov ax, 0x0e45             ; BIOS teletype: E, then halt.
    xor bx, bx
    int 0x10
    cli
.halt:
    hlt
    jmp .halt
boot_drive db 0
spt db 0
retries db 0
times 510 - ($ - $$) db 0
dw 0xAA55
