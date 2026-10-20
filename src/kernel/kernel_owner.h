#ifndef KERNEL_OWNER_H
#define KERNEL_OWNER_H
/* Central private built-in owner domain, never a public ABI handle. Allocation
 * is serialized, bounded and nonreusing. Zero means lifetime capacity exhausted. */
unsigned kernel_owner_allocate(void);
#endif
