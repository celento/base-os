; nasm -f bin examples/hello.asm -o build/hello.bex
bits 32
org 0
dd 0x31584542, start, image_end, 0
start:
    mov eax, 1                  ; write(offset, length)
    mov ebx, message
    mov ecx, message_end-message
    int 0x80
    mov eax, 2                  ; plot(x, y, palette index)
    mov ebx, 20
    mov ecx, 20
    mov edx, 48
    int 0x80
    xor eax, eax                ; exit(0)
    xor ebx, ebx
    int 0x80
message: db 'Hello from an isolated native program!',10
message_end:
image_end:
