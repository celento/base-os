#ifndef VM_H
#define VM_H
#include <stdint.h>
#include "../sdk/baseos_abi.h"
#include "../sdk/baseos_sys.h"

/* Private address spaces for ELF processes. Each space has its own page
 * directory: the kernel's supervisor identity mappings everywhere except the
 * user window [BOS_USER_BASE, BOS_USER_END), which holds only this process's
 * pages. Areas reserve address ranges; frames arrive on first touch (demand
 * zero) and belong to the space's owner handle in physmem. Unreserved
 * addresses, including the page below the stack, fault.
 *
 * Serialized kernel context only (the desktop thread or a thread's syscall),
 * never interrupt handlers other than the page-fault path. */
typedef struct VmArea {
    uint32_t start, end;   /* page aligned, end exclusive */
    unsigned prot;         /* BOS_PROT_* */
    struct VmArea *next;
} VmArea;

typedef struct {
    uint32_t directory;    /* physical page directory */
    BosHandle owner;       /* owns every frame of this space */
    VmArea *areas;         /* sorted, non-overlapping */
    uint32_t reserved_bytes, resident_pages, table_pages;
} VmSpace;

#define VM_PAGE 4096u
#define VM_MMAP_TOP 0xBF000000u   /* floating mappings grow down from here */

/* 0 or a negated BOS_E* error. */
int vm_create(VmSpace *space, BosHandle owner);
/* The space must not be active. Releases every frame of its owner. */
void vm_destroy(VmSpace *space);
/* Load this space's root (or the kernel root for 0). */
void vm_activate(const VmSpace *space);
uint32_t vm_kernel_root(void);

/* Reserve [address, address + length). Without fixed, address is a hint and
 * the result is the highest free range below VM_MMAP_TOP. Fixed replaces any
 * existing mapping in the range. Returns 0 and *out, or a negated error. */
int vm_map(VmSpace *space, uint32_t address, uint32_t length, unsigned prot, int fixed,
           uint32_t *out);
int vm_unmap(VmSpace *space, uint32_t address, uint32_t length);
int vm_protect(VmSpace *space, uint32_t address, uint32_t length, unsigned prot);

/* Resolve a fault at address. write/user come from the CPU error code.
 * Returns 1 if a page was mapped, 0 if the access is invalid. */
int vm_fault(VmSpace *space, uint32_t address, int write);

/* Validate that [address, address + length) lies in areas allowing the access
 * and make every page present, so the caller can copy directly while this
 * space is active. Returns 0 or -BOS_EFAULT (or -BOS_ENOMEM). */
int vm_prepare(VmSpace *space, uint32_t address, uint32_t length, int write);
/* Copy a NUL-terminated user string of at most capacity - 1 bytes. Returns
 * its length, -BOS_EFAULT or -BOS_ENAMETOOLONG. */
/* Physical address behind a present user address, or 0. */
uint32_t vm_physical(VmSpace *space, uint32_t address);
int vm_string(VmSpace *space, uint32_t address, char *out, uint32_t capacity);
#endif
