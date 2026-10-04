bits 32
section .text
global process_enter, process_resume, process_leave
extern process_saved_sp, process_result
process_enter:
    pushfd
    cli                          ; frame/segment switching must be atomic
    pushad
    mov [process_saved_sp], esp
    mov eax, [esp+40]             ; entry offset, after saved regs/flags/return
    mov dx, 0x23
    mov ds, dx
    mov es, dx
    mov fs, dx
    mov gs, dx
    push dword 0x23
    push dword USER_CAPACITY-16
    push dword 0x202              ; IF=1, IOPL=0, NT/TF/DF clear
    push dword 0x1b
    push eax
    iretd
; The saved interrupt frame lives in supervisor-only task memory. SS still
; names the kernel segment while restoring DS/ES/FS/GS and the IRET frame.
process_resume:
    pushfd
    cli                          ; frame/segment switching must be atomic
    pushad
    mov [process_saved_sp], esp
    mov esp, [esp+40]
    popad
    pop gs
    pop fs
    pop es
    pop ds
    add esp, 8
    iretd
process_leave:
    cli
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, [process_saved_sp]
    popad
    popfd
    mov eax, [process_result]
    ret
section .note.GNU-stack noalloc noexec nowrite progbits
