bits 32
org 0
    dd 0x31584542, entry, file_end, 0
entry:
    fninit
    fild dword [number]
.loop:
    mov eax, 10
    xor ebx, ebx
    int 0x80
    test eax, eax
    jnz .bad
    fist dword [actual]
    mov ebx, [actual]
    cmp ebx, VALUE
    jne .bad
    mov eax, 4
    int 0x80
    cmp eax, 'q'
    jne .loop
    xor eax, eax
    xor ebx, ebx
    int 0x80
.bad:
    xor eax, eax
    mov ebx, 77
    int 0x80
number: dd VALUE
actual: dd 0
file_end:
