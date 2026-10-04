bits 32
org 0
    dd 0x31584542, entry, file_end, 0
entry:
    mov eax, 11
    mov ebx, 1000
    int 0x80
    test eax, eax
    jnz .bad
    mov eax, 4
    int 0x80
    cmp eax, 'x'
    jne .bad
    mov eax, 10
    int 0x80                    ; EBX is irrelevant for yield
    test eax, eax
    jnz .bad
    xor eax, eax
    xor ebx, ebx
    int 0x80
.bad:
    xor eax, eax
    mov ebx, 88
    int 0x80
file_end:
