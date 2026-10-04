; Ordinary 640 KiB initialized image and BSS fixture for relocation checks.
; No patched instructions, CPU-fault injection, or malformed guest data.
bits 32
section .data
align 16
global kernel_tail_padding, kernel_tail_marker
kernel_tail_padding:
    times KERNEL_TAIL_PADDING db 0x5A
kernel_tail_marker:
    db 'BASEOS-HIGH-KERNEL-END'
    times 32 - ($ - kernel_tail_marker) db 0
section .bss
align 16
global kernel_growth_bss, kernel_growth_bss_end
kernel_growth_bss:
    resb 0x40000
kernel_growth_bss_end:
section .note.GNU-stack noalloc noexec nowrite progbits
