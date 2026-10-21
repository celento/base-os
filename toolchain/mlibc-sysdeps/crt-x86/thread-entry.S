// New threads start here with ESP at the words PrepareStack wrote:
// entry, tcb, argument. Passing them on keeps the 16-byte call alignment.
	.section .text
	.global __mlibc_thread_entry
	.type __mlibc_thread_entry, @function
__mlibc_thread_entry:
	pop %ecx
	pop %edx
	pop %eax
	push %eax
	push %edx
	push %ecx
	call __mlibc_thread_trampoline
	ud2
	.section .note.GNU-stack,"",%progbits
