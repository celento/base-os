; A small real-mode DOS program for testing DOSBox on BaseOS.
; Prints a line through DOS, writes RESULT.TXT, then switches to VGA mode 13h,
; fills the screen with palette entry 1 set to a known color, draws a white
; bar and waits for a key before returning to text mode and exiting.
;
;   nasm -f bin examples/dos/vgatest.asm -o VGATEST.COM
        org 0x100

        mov dx, hello
        mov ah, 0x09
        int 0x21

        mov ah, 0x3c            ; create RESULT.TXT
        xor cx, cx
        mov dx, filename
        int 0x21
        jc .skip_file
        mov bx, ax
        mov ah, 0x40
        mov cx, result_len
        mov dx, result
        int 0x21
        mov ah, 0x3e
        int 0x21
.skip_file:

        mov ax, 0x0013
        int 0x10
        mov dx, 0x3c8           ; DAC entry 1 = 6-bit (4, 13, 21)
        mov al, 1
        out dx, al
        inc dx
        mov al, 4
        out dx, al
        mov al, 13
        out dx, al
        mov al, 21
        out dx, al

        mov ax, 0xa000
        mov es, ax
        xor di, di
        mov cx, 320 * 200 / 2
        mov ax, 0x0101
        rep stosw
        mov di, 320 * 90        ; white bar across the middle
        mov cx, 320 * 20 / 2
        mov ax, 0x0f0f
        rep stosw

        xor ah, ah              ; wait for a key
        int 0x16
        mov [key], al

        mov ax, 0x0003
        int 0x10
        mov dx, bye
        mov ah, 0x09
        int 0x21
        mov ax, 0x4c00
        int 0x21

hello   db 'VGATEST running under DOS', 13, 10, '$'
bye     db 'VGATEST done', 13, 10, '$'
filename db 'RESULT.TXT', 0
result  db 'VGATEST OK', 13, 10
result_len equ $ - result
key     db 0
