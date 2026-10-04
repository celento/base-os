; kernel_entry.asm
; Linked at KERNEL_LOAD_ADDR (1 MiB), initially staged at KERNEL_STAGE_ADDR.
; The BIOS far-jumps to the staged copy in 16-bit real mode. Relative calls
; within this bootstrap work in either copy; absolute code/GDT addresses must
; explicitly use the staged address until the protected-mode copy completes.
; Collect low-memory boot info, enter PM, unpack, then run the linked kernel.

section .text.entry
[bits 16]
global _start
extern kmain
extern __load_end, __bss_start, __bss_end
extern platform_init

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

    sti

    call collect_memory_map
    call vbe_try
.have_mode:
    cli

    ; Enable and verify A20 after the final BIOS mode call, before any copy,
    ; stack access or instruction fetch in extended RAM. Both bytes are restored.
    in al, 0x92
    and al, 0xFE               ; never assert the fast reset bit
    or al, 2
    out 0x92, al
    mov ax, 0xFFFF
    mov fs, ax
    mov al, [0x500]
    mov ah, [fs:0x510]         ; FFFF:0510 = physical 0x100500
    mov byte [0x500], 0
    mov byte [fs:0x510], 0xFF
    cmp byte [0x500], 0
    mov [fs:0x510], ah
    mov [0x500], al
    jne a20_failed
    xor ax, ax
    mov fs, ax

    ; The initial GDTR points into the staged, not yet relocated, image.
    mov word [0x7E40], gdt_end - gdt_start - 1
    mov dword [0x7E42], gdt_start - KERNEL_LOAD_ADDR + KERNEL_STAGE_ADDR
    lgdt [0x7E40]

    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp dword CODE_SEG:(pm_relocate - KERNEL_LOAD_ADDR + KERNEL_STAGE_ADDR)

a20_failed:
    ; C and its serial/panic routines are not available until relocation.
    mov si, a20_error - _start
.print:
    cs lodsb
    test al, al
    jz .halt
    mov ah, 0x0E
    mov bx, 7
    int 0x10
    cld
    jmp .print
.halt:
    cli
    hlt
    jmp .halt
a20_error db "A20 unavailable", 0

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
pm_relocate:
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    ; An explicit low stack avoids inherited upper ESP bits. It is disjoint
    ; from BIOS scratch, the staged source and the high reconstructed image.
    mov esp, KERNEL_BOOT_STACK_TOP
    cld

    mov ebp, KERNEL_STAGE_ADDR + KERNEL_BOOTSTRAP_BYTES
    cmp dword [ebp], KERNEL_PACK_MAGIC
    jne packed_failed
    cmp dword [ebp + 4], KERNEL_PACK_HEADER_BYTES
    jne packed_failed
    cmp dword [ebp + 8], 1       ; codec 0 = raw, 1 = LZ4 block
    ja packed_failed
    cmp dword [ebp + 24], 0
    jne packed_failed
    mov ecx, [ebp + 12]
    test ecx, ecx
    jz packed_failed
    cmp ecx, KERNEL_SECTORS * SECTOR_SIZE - KERNEL_BOOTSTRAP_BYTES - KERNEL_PACK_HEADER_BYTES
    ja packed_failed
    mov eax, __load_end
    sub eax, KERNEL_LOAD_ADDR
    cmp [ebp + 16], eax         ; exact size from this linked bootstrap
    jne packed_failed
    test eax, eax
    jz packed_failed
    cmp eax, STACK_BOTTOM - KERNEL_LOAD_ADDR
    ja packed_failed
    mov esi, ebp
    mov ecx, 28
    call packed_crc32
    cmp eax, [ebp + 28]
    jne packed_failed

    lea esi, [ebp + KERNEL_PACK_HEADER_BYTES]
    mov edi, KERNEL_LOAD_ADDR
    mov ebx, __load_end         ; fixed exclusive destination bound
    cmp dword [ebp + 8], 0
    je .raw
    mov eax, [ebp + 12]
    lea ebp, [esi + eax]        ; exclusive input bound, already stage-bounded
.sequence:
    cmp esi, ebp
    jae packed_failed
    movzx edx, byte [esi]
    inc esi
    mov ecx, edx
    shr ecx, 4
    call packed_length
    mov eax, ebp
    sub eax, esi
    cmp ecx, eax
    ja packed_failed           ; literal read must stay in payload
    mov eax, ebx
    sub eax, edi
    cmp ecx, eax
    ja packed_failed           ; literal write must stay in raw image
    rep movsb
    cmp esi, ebp
    je .decoded                ; final sequence ends after literals
    mov eax, ebp
    sub eax, esi
    cmp eax, 2
    jb packed_failed
    mov ecx, edx
    and ecx, 15
    movzx edx, word [esi]
    add esi, 2
    test edx, edx
    jz packed_failed
    mov eax, edi
    sub eax, KERNEL_LOAD_ADDR
    cmp edx, eax
    ja packed_failed           ; match cannot start before produced output
    call packed_length
    add ecx, 4
    jc packed_failed
    mov eax, ebx
    sub eax, edi
    cmp ecx, eax
    ja packed_failed
    push esi
    mov esi, edi
    sub esi, edx
    rep movsb                  ; forward byte copy supports overlapping matches
    pop esi
    jmp .sequence
.raw:
    mov ecx, [ebp + 12]
    cmp ecx, [ebp + 16]
    jne packed_failed
    rep movsb
.decoded:
    cmp edi, ebx
    jne packed_failed
    mov esi, KERNEL_LOAD_ADDR
    mov ecx, edi
    sub ecx, esi
    call packed_crc32
    cmp eax, [KERNEL_STAGE_ADDR + KERNEL_BOOTSTRAP_BYTES + 20]
    jne packed_failed

    ; process_init will edit the reconstructed user/TSS descriptors. Reload
    ; GDTR before C so LTR and later ring-3 returns see those same bytes.
    mov dword [0x7E42], gdt_start
    lgdt [0x7E40]
    jmp CODE_SEG:pm_start

; Extend an LZ4 nibble length in ECX. ESI/EBP bound every extra-byte read.
; EDX (token or match offset) and EBX/EDI (destination bounds) are preserved.
packed_length:
    cmp ecx, 15
    jne .done
.more:
    cmp esi, ebp
    jae packed_failed
    movzx eax, byte [esi]
    inc esi
    add ecx, eax
    jc packed_failed
    cmp ecx, STACK_BOTTOM - KERNEL_LOAD_ADDR
    ja packed_failed
    cmp eax, 255
    je .more
.done:
    ret

; IEEE CRC32, same polynomial and initial/final XOR as Python zlib.crc32.
; Reads exactly ECX bytes from ESI, returns EAX, clobbers EDX only otherwise.
packed_crc32:
    mov eax, 0xFFFFFFFF
.byte:
    test ecx, ecx
    jz .done
    xor al, [esi]
    inc esi
    mov edx, 8
.bit:
    shr eax, 1
    jnc .next
    xor eax, 0xEDB88320
.next:
    dec edx
    jnz .bit
    dec ecx
    jmp .byte
.done:
    not eax
    ret

packed_failed:
    ; Emit a bounded early COM1 diagnostic without calling C or using RAM
    ; outside the bootstrap reservation. No writes continue after failure.
    cli
    mov dx, 0x3F9
    xor al, al
    out dx, al
    mov dx, 0x3FB
    mov al, 0x80
    out dx, al
    mov dx, 0x3F8
    mov al, 1
    out dx, al
    mov dx, 0x3F9
    xor al, al
    out dx, al
    mov dx, 0x3FB
    mov al, 3
    out dx, al
    mov esi, packed_error - KERNEL_LOAD_ADDR + KERNEL_STAGE_ADDR
.print:
    lodsb
    test al, al
    jz .halt
    mov bl, al
    mov dx, 0xE9
    out dx, al
    mov ecx, 65536
.wait:
    mov dx, 0x3FD
    in al, dx
    test al, 0x20
    jnz .send
    loop .wait
    jmp .print
.send:
    mov dx, 0x3F8
    mov al, bl
    out dx, al
    jmp .print
.halt:
    hlt
    jmp .halt
packed_error db 'PACKED KERNEL ERROR', 13, 10, 0

pm_start:
    mov esp, STACK_TOP
    xor ebp, ebp
    cld
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
global __bootstrap_end
__bootstrap_end:

CODE_SEG equ gdt_code - gdt_start
DATA_SEG equ gdt_data - gdt_start

section .note.GNU-stack noalloc noexec nowrite progbits
