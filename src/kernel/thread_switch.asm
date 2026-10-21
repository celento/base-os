bits 32
section .text
global thread_switch
; thread_switch(uint32_t *save, uint32_t next): saves the callee-saved
; registers and flags on this stack, stores its top in *save and resumes the
; stack at next, which was saved the same way (or built by thread.c).
thread_switch:
    mov eax, [esp+4]
    mov edx, [esp+8]
    pushfd
    push ebp
    push ebx
    push esi
    push edi
    mov [eax], esp
    mov esp, edx
    pop edi
    pop esi
    pop ebx
    pop ebp
    popfd
    ret
section .note.GNU-stack noalloc noexec nowrite progbits
