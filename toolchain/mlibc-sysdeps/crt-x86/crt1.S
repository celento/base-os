/* BaseOS process entry: the kernel starts each program here with the System V
 * initial stack (argc, argv, envp, auxv) at %esp and every other register zero. */
.section .text
.global _start
.type _start, %function
_start:
	xor %ebp, %ebp
	mov %esp, %ecx
	and $-16, %esp
	sub $8, %esp
	push %ecx
	push $main
	call __mlibc_entry
	ud2
.size _start, . - _start

.section .note.GNU-stack,"",%progbits
