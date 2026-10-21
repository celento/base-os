bits 32
section .text
extern interrupt_dispatch
%assign vector 0
%rep 256
isr_%+vector:
%if vector != 8 && vector != 10 && vector != 11 && vector != 12 && vector != 13 && vector != 14 && vector != 17 && vector != 21 && vector != 29 && vector != 30
    push dword 0
%endif
    push dword vector
    jmp interrupt_common
%assign vector vector+1
%endrep
interrupt_common:
    cld
    push ds
    push es
    push fs
    push gs
    pushad
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    ; Handlers use kernel segments; restore user segments on return.
    ; Preserve the original frame and align the stack for the C ABI.
    mov ebx, esp
    and esp, -16
    sub esp, 12
    push ebx
    call interrupt_dispatch
    mov esp, ebx
    popad
    pop gs
    pop fs
    pop es
    pop ds
    add esp, 8
    iretd
section .rodata
align 4
global isr_table
isr_table:
%assign vector 0
%rep 256
    dd isr_%+vector
%assign vector vector+1
%endrep
section .note.GNU-stack noalloc noexec nowrite progbits
