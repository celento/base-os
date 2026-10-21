/* long __baseos_syscall(long number, long a, long b, long c, long d, long e, long f)
 * Arguments travel in EBX, ECX, EDX, ESI, EDI and EBP; see sdk/baseos_sys.h. */
.section .text
.global __baseos_syscall
.hidden __baseos_syscall
.type __baseos_syscall, %function
__baseos_syscall:
	push %ebp
	push %edi
	push %esi
	push %ebx
	mov 20(%esp), %eax
	mov 24(%esp), %ebx
	mov 28(%esp), %ecx
	mov 32(%esp), %edx
	mov 36(%esp), %esi
	mov 40(%esp), %edi
	mov 44(%esp), %ebp
	int $0x81
	pop %ebx
	pop %esi
	pop %edi
	pop %ebp
	ret
.size __baseos_syscall, . - __baseos_syscall

.section .note.GNU-stack,"",%progbits
