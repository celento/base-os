; kernel_entry.asm
; Linked at 0x10000. Bootloader far-jumps here in 16-bit real mode.
; Set VBE 1280x720 (or HD-ish fallback), stash boot-info at 0x7E00, enter PM.

section .text.entry
[bits 16]
global _start
extern kmain
extern __bss_start, __bss_end
extern platform_init, panic

BOOTINFO        equ BOOTINFO_ADDR
VBE_INFO        equ 0x8000
MODE_INFO       equ 0x8200
BEST            equ 0x8300          ; scratch: score, mode, w, h, pitch, bpp, lfb

; BEST layout
; 0  dw score
; 2  dw mode
; 4  dw width
; 6  dw height
; 8  dw pitch
; 10 db bpp
; 11 db pad
; 12 dd lfb

_start:
    cli
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    mov sp, 0x7C00
    push dx                    ; boot drive and sectors per track

    ; Clear boot-info + scratch
    mov di, BOOTINFO
    xor ax, ax
    mov cx, 0x80
    rep stosw
    pop dx
    mov [BOOTINFO + 18], dl
    mov [BOOTINFO + 19], dh

    ; Fast A20 so we can use extended RAM (backbuffer at 2MB)
    in al, 0x92
    and al, 0xFE               ; never assert the fast reset bit
    or al, 2
    out 0x92, al

    sti

    call collect_memory_map
    call vbe_try
.have_mode:
    cli

    ; Build GDTR at 0x7E40 with a 32-bit linear GDT address
    mov word [0x7E40], gdt_end - gdt_start - 1
    mov dword [0x7E42], gdt_start
    lgdt [0x7E40]

    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp dword CODE_SEG:pm_start

; ---------- VBE: find 1280x720 (prefer 32bpp), else 1280x800, else 1024x768 ----------
; CF=1 on success
vbe_try:
    pusha
    mov di, VBE_INFO
    mov cx, 256
    xor ax, ax
    rep stosw
    mov di, VBE_INFO
    mov dword [di], 0x32454256      ; 'VBE2'
    mov ax, 0x4F00
    int 0x10
    cld
    cmp ax, 0x004F
    jne .fail
    cmp dword [VBE_INFO], 0x41534556 ; 'VESA'
    jne .fail

    ; zero BEST scratch (score/mode/geom/lfb)
    mov di, BEST
    xor ax, ax
    mov cx, 16
    rep stosw
    xor ax, ax
    mov es, ax

    mov ax, [VBE_INFO + 14]         ; mode list offset
    mov si, ax
    mov ax, [VBE_INFO + 16]         ; mode list segment
    mov fs, ax
    xor bx, bx                      ; safety counter

.walk:
    inc bx
    cmp bx, 256
    ja .picked
    mov cx, [fs:si]
    cmp cx, 0xFFFF
    je .picked
    add si, 2

    ; Skip obviously invalid entries (keep 0x100+ VBE modes)
    cmp cx, 0x0100
    jb .walk

    mov [BEST + 16], cx             ; remember mode (BIOS may clobber CX)

    ; 4F01 get mode info
    push bx
    push si
    mov di, MODE_INFO
    mov cx, 128
    xor ax, ax
    rep stosw
    mov di, MODE_INFO
    mov cx, [BEST + 16]
    mov ax, 0x4F01
    int 0x10
    cld
    pop si
    pop bx
    cmp ax, 0x004F
    jne .walk
    mov cx, [BEST + 16]

    mov ax, [MODE_INFO]             ; attributes
    and ax, 0x0091                  ; require all attributes
    cmp ax, 0x0091
    jne .walk
    cmp byte [MODE_INFO + 27], 6     ; direct color
    jne .walk
    cmp dword [MODE_INFO + 40], 0
    je .walk

    mov al, [MODE_INFO + 25]        ; bpp
    cmp al, 16
    je .bpp_ok
    cmp al, 24
    je .bpp_ok
    cmp al, 32
    je .bpp_ok
    jmp .walk
.bpp_ok:
    cmp word [VBE_INFO + 4], 0x0300
    jb .masks
    mov ax, [MODE_INFO + 50]
    mov [MODE_INFO + 16], ax
    mov eax, [MODE_INFO + 54]
    mov [MODE_INFO + 31], eax
    mov ax, [MODE_INFO + 58]
    mov [MODE_INFO + 35], ax
.masks:
    cmp byte [MODE_INFO + 25], 16
    jne .rgb888
    cmp word [MODE_INFO + 31], 0x0B05
    jne .walk
    cmp word [MODE_INFO + 33], 0x0506
    jne .walk
    cmp word [MODE_INFO + 35], 0x0005
    jne .walk
    jmp .geometry
.rgb888:
    cmp word [MODE_INFO + 31], 0x1008
    jne .walk
    cmp word [MODE_INFO + 33], 0x0808
    jne .walk
    cmp word [MODE_INFO + 35], 0x0008
    jne .walk
.geometry:
    movzx eax, byte [MODE_INFO + 25]
    shr eax, 3
    movzx edx, word [MODE_INFO + 18]
    imul eax, edx
    movzx edx, word [MODE_INFO + 16]
    cmp edx, eax
    jb .walk
    mov dx, [MODE_INFO + 18]        ; width
    mov di, [MODE_INFO + 20]        ; height
    xor ax, ax                      ; score
    cmp dx, 1280
    jne .chk800
    cmp di, 720
    jne .chk800
    mov ax, 300
    jmp .score
.chk800:
    cmp dx, 1280
    jne .chk1024
    cmp di, 800
    jne .chk1024
    mov ax, 200
    jmp .score
.chk1024:
    cmp dx, 1024
    jne .walk
    cmp di, 768
    jne .walk
    mov ax, 100
.score:
    xor dx, dx
    mov dl, [MODE_INFO + 25]
    add ax, dx
    cmp ax, [BEST]
    jbe .walk

    ; save winner
    mov [BEST], ax
    mov [BEST + 2], cx              ; mode
    mov ax, [MODE_INFO + 18]
    mov [BEST + 4], ax
    mov ax, [MODE_INFO + 20]
    mov [BEST + 6], ax
    mov ax, [MODE_INFO + 16]        ; pitch (banked)
    mov [BEST + 8], ax
    mov al, [MODE_INFO + 25]
    mov [BEST + 10], al
    mov eax, [MODE_INFO + 40]
    mov [BEST + 12], eax
    mov eax, [MODE_INFO + 31]
    mov [BEST + 18], eax
    mov ax, [MODE_INFO + 35]
    mov [BEST + 22], ax
    jmp .walk

.picked:
    cmp word [BEST], 0
    je .fail

    ; Set mode with LFB bit
    mov bx, [BEST + 2]
    and bx, 0x3FFF
    or bx, 0x4000
    mov ax, 0x4F02
    int 0x10
    cld
    cmp ax, 0x004F
    jne .fail

    call store_best
    mov byte [BOOTINFO + 15], 1     ; flags: VBE
    popa
    stc
    ret
.fail:
    popa
    clc
    ret

store_best:
    mov dword [BOOTINFO], BOOTINFO_MAGIC
    mov eax, [BEST + 12]
    mov [BOOTINFO + 4], eax         ; lfb
    mov ax, [BEST + 4]
    mov [BOOTINFO + 8], ax          ; width
    mov ax, [BEST + 6]
    mov [BOOTINFO + 10], ax         ; height
    mov ax, [BEST + 8]
    mov [BOOTINFO + 12], ax         ; pitch
    mov al, [BEST + 10]
    mov [BOOTINFO + 14], al         ; bpp
    mov eax, [BEST + 18]
    mov [BOOTINFO + 20], eax
    mov ax, [BEST + 22]
    mov [BOOTINFO + 24], ax
    ret

 ; E820 descriptors live below the boot stack and VBE scratch.
collect_memory_map:
    xor ebx, ebx
    mov di, E820_BASE
    mov word [BOOTINFO + 16], 0
.next:
    cmp word [BOOTINFO + 16], E820_MAX
    jae .fail
    mov dword [es:di + 20], 1
    mov eax, 0xE820
    mov edx, 0x534D4150
    mov ecx, 24
    push di
    int 0x15
    cld
    pop di
    jc .fail
    cmp eax, 0x534D4150
    jne .fail
    cmp ecx, 20
    jb .fail
    inc word [BOOTINFO + 16]
    add di, 24
    test ebx, ebx
    jnz .next
    ret
.fail:
    mov word [BOOTINFO + 16], 0
    ret

[bits 32]
pm_start:
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, STACK_TOP
    cld
    ; Verify A20 before accessing extended RAM. Restore both probe bytes.
    mov esi, 0x500
    mov edi, 0x100500
    mov al, [esi]
    mov ah, [edi]
    mov byte [esi], 0
    mov byte [edi], 0xFF
    cmp byte [esi], 0
    mov [edi], ah
    mov [esi], al
    setne bl
%ifdef TEST_DIRTY_BSS
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi
    mov al, 0xA5
    rep stosb
%endif
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi
    xor eax, eax
    rep stosb
    call platform_init
    test bl, bl
    jz .a20_ok
    sub esp, 12
    push dword a20_error
    call panic
.a20_ok:
    call kmain
.hang:
    hlt
    jmp .hang

align 8
gdt_start:
    dq 0
gdt_code:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 0x9A
    db 0xCF
    db 0x00
gdt_data:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 0x92
    db 0xCF
    db 0x00
global gdt_user_code, gdt_user_data, gdt_tss
gdt_user_code: dq 0
gdt_user_data: dq 0
gdt_tss: dq 0
gdt_end:

CODE_SEG equ gdt_code - gdt_start
DATA_SEG equ gdt_data - gdt_start

section .rodata
a20_error db "A20 is unavailable", 0

section .note.GNU-stack noalloc noexec nowrite progbits
