bits 32
section .text
global process_enter, process_leave
extern process_saved_sp, process_result
process_enter:
    pushfd
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
