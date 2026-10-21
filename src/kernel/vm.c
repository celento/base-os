#include "kernel/vm.h"
#include "kernel/kheap.h"
#include "kernel/physmem.h"
#include "kernel/platform.h"
#include "kernel/layout.h"
#include "kernel/fs.h"
#include "kernel/program.h"

#define PTE_PRESENT 1u
#define PTE_WRITE 2u
#define PTE_USER 4u
#define FIRST_SLOT (BOS_USER_BASE >> 22)
#define END_SLOT (BOS_USER_END >> 22)

static const VmSpace *active_space;

static uint32_t *directory_of(const VmSpace *space) {
    return (uint32_t *)(uintptr_t)space->directory;
}

static void invalidate(const VmSpace *space, uint32_t address) {
    if (space == active_space) __asm__ volatile("invlpg (%0)" :: "r"(address) : "memory");
}

static int page_aligned(uint32_t value) { return !(value & (VM_PAGE - 1)); }

/* Range inside the user window; length is nonzero and page aligned. */
static int range_valid(uint32_t address, uint32_t length) {
    return length && page_aligned(address) && page_aligned(length) &&
           address >= BOS_USER_BASE && address < BOS_USER_END &&
           length <= BOS_USER_END - address;
}

uint32_t vm_kernel_root(void) { return KERNEL_DIRECTORY_BASE; }

int vm_create(VmSpace *space, BosHandle owner) {
    if (!space || owner == BOS_HANDLE_INVALID) return -BOS_EINVAL;
    kmemset(space, 0, sizeof *space);
    process_paging_init();
    uint32_t frame;
    if (physmem_alloc_any(owner, PHYS_PROCESS_TABLE, 1, &frame, PHYS_KERNEL_LIMIT) != PHYS_OK)
        return -BOS_ENOMEM;
    uint32_t *directory = (uint32_t *)(uintptr_t)frame;
    const uint32_t *kernel = (const uint32_t *)(uintptr_t)KERNEL_DIRECTORY_BASE;
    for (unsigned slot = 0; slot < 1024; ++slot)
        directory[slot] = slot >= FIRST_SLOT && slot < END_SLOT ? 0 : kernel[slot];
    space->directory = frame;
    space->owner = owner;
    space->table_pages = 1;
    return 0;
}

void vm_destroy(VmSpace *space) {
    if (!space || !space->directory) return;
    if (space == active_space) panic("vm: destroying the active space");
    for (VmArea *area = space->areas, *next; area; area = next) {
        next = area->next;
        kfree(area);
    }
    if (physmem_release_owner(space->owner) != PHYS_OK) panic("vm: owner release");
    kmemset(space, 0, sizeof *space);
}

void vm_activate(const VmSpace *space) {
    uint32_t root = space ? space->directory : KERNEL_DIRECTORY_BASE;
    active_space = space;
    __asm__ volatile("mov %0,%%cr3" :: "r"(root) : "memory");
}

/* Page table entry for address, or 0. With create, missing tables are
 * allocated (zeroed); a 0 result then means out of memory. */
static uint32_t *pte_of(VmSpace *space, uint32_t address, int create) {
    uint32_t *slot = &directory_of(space)[address >> 22];
    if (!(*slot & PTE_PRESENT)) {
        if (!create) return 0;
        uint32_t frame;
        if (physmem_alloc_any(space->owner, PHYS_PROCESS_TABLE, 1, &frame, PHYS_KERNEL_LIMIT) != PHYS_OK)
            return 0;
        *slot = frame | PTE_PRESENT | PTE_WRITE | PTE_USER;
        ++space->table_pages;
    }
    uint32_t *table = (uint32_t *)(uintptr_t)(*slot & ~(VM_PAGE - 1));
    return &table[(address >> 12) & 1023];
}

static VmArea *area_at(const VmSpace *space, uint32_t address) {
    for (VmArea *area = space->areas; area && area->start <= address; area = area->next)
        if (address < area->end) return area;
    return 0;
}

/* Drop any frames mapped in [start, end). */
static void release_pages(VmSpace *space, uint32_t start, uint32_t end) {
    uint32_t frames[64];
    unsigned batch = 0;
    for (uint32_t address = start; address < end; address += VM_PAGE) {
        if (!(directory_of(space)[address >> 22] & PTE_PRESENT)) {
            /* Skip to the next table, staying aligned and in range. */
            uint32_t next = (address | 0x3FFFFFu) + 1;
            if (!next || next >= end) break;
            address = next - VM_PAGE;
            continue;
        }
        uint32_t *pte = pte_of(space, address, 0);
        if (!(*pte & PTE_PRESENT)) continue;
        frames[batch++] = *pte & ~(VM_PAGE - 1);
        *pte = 0;
        invalidate(space, address);
        --space->resident_pages;
        if (batch == 64) {
            if (physmem_release(space->owner, PHYS_PROCESS_PAGE, frames, batch) != PHYS_OK)
                panic("vm: page release");
            batch = 0;
        }
    }
    if (batch && physmem_release(space->owner, PHYS_PROCESS_PAGE, frames, batch) != PHYS_OK)
        panic("vm: page release");
}

/* Split areas so that start and end fall on area boundaries. Returns 0 or
 * -BOS_ENOMEM before changing anything that matters (a split alone is
 * invisible). */
static int split_at(VmSpace *space, uint32_t address) {
    VmArea *area = area_at(space, address);
    if (!area || area->start == address) return 0;
    VmArea *tail = kmalloc(sizeof *tail);
    if (!tail) return -BOS_ENOMEM;
    tail->start = address;
    tail->end = area->end;
    tail->prot = area->prot;
    tail->next = area->next;
    area->end = address;
    area->next = tail;
    return 0;
}

/* Merge neighbours with equal protection to keep the list short. */
static void coalesce(VmSpace *space) {
    for (VmArea *area = space->areas; area && area->next;) {
        VmArea *next = area->next;
        if (area->end == next->start && area->prot == next->prot) {
            area->end = next->end;
            area->next = next->next;
            kfree(next);
        } else {
            area = next;
        }
    }
}

static int range_free(const VmSpace *space, uint32_t start, uint32_t end) {
    for (const VmArea *area = space->areas; area && area->start < end; area = area->next)
        if (area->end > start) return 0;
    return 1;
}

/* Highest gap of length bytes below VM_MMAP_TOP. */
static int find_gap(const VmSpace *space, uint32_t length, uint32_t *out) {
    uint32_t best = 0;
    int found = 0;
    uint32_t gap_start = BOS_USER_BASE;
    const VmArea *area = space->areas;
    for (;;) {
        uint32_t gap_end = area ? area->start : BOS_USER_END;
        if (gap_end > VM_MMAP_TOP) gap_end = VM_MMAP_TOP;
        if (gap_end > gap_start && gap_end - gap_start >= length) {
            best = gap_end - length;
            found = 1;
        }
        if (!area) break;
        if (area->end > gap_start) gap_start = area->end;
        if (gap_start >= VM_MMAP_TOP) break;
        area = area->next;
    }
    if (found) *out = best;
    return found;
}

static int remove_range(VmSpace *space, uint32_t start, uint32_t end) {
    if (split_at(space, start) || split_at(space, end)) return -BOS_ENOMEM;
    VmArea **link = &space->areas;
    while (*link) {
        VmArea *area = *link;
        if (area->start >= start && area->end <= end) {
            *link = area->next;
            space->reserved_bytes -= area->end - area->start;
            kfree(area);
        } else {
            link = &area->next;
        }
    }
    release_pages(space, start, end);
    return 0;
}

int vm_map(VmSpace *space, uint32_t address, uint32_t length, unsigned prot, int fixed,
           uint32_t *out) {
    if (!space || !out || !length || prot & ~(BOS_PROT_READ | BOS_PROT_WRITE | BOS_PROT_EXEC))
        return -BOS_EINVAL;
    if (length > BOS_USER_END - BOS_USER_BASE) return -BOS_ENOMEM;
    length = (length + VM_PAGE - 1) & ~(VM_PAGE - 1);
    if (fixed) {
        if (!range_valid(address, length)) return -BOS_EINVAL;
    } else if (!(address && range_valid(address & ~(VM_PAGE - 1), length) &&
                 range_free(space, address & ~(VM_PAGE - 1), (address & ~(VM_PAGE - 1)) + length))) {
        if (!find_gap(space, length, &address)) return -BOS_ENOMEM;
    } else {
        address &= ~(VM_PAGE - 1);
    }
    VmArea *fresh = kmalloc(sizeof *fresh);
    if (!fresh) return -BOS_ENOMEM;
    if (fixed && remove_range(space, address, address + length)) {
        kfree(fresh);
        return -BOS_ENOMEM;
    }
    fresh->start = address;
    fresh->end = address + length;
    fresh->prot = prot;
    VmArea **link = &space->areas;
    while (*link && (*link)->start < address) link = &(*link)->next;
    fresh->next = *link;
    *link = fresh;
    space->reserved_bytes += length;
    coalesce(space);
    *out = address;
    return 0;
}

int vm_unmap(VmSpace *space, uint32_t address, uint32_t length) {
    if (!space || !page_aligned(address)) return -BOS_EINVAL;
    length = (length + VM_PAGE - 1) & ~(VM_PAGE - 1);
    if (!range_valid(address, length)) return -BOS_EINVAL;
    int result = remove_range(space, address, address + length);
    coalesce(space);
    return result;
}

int vm_protect(VmSpace *space, uint32_t address, uint32_t length, unsigned prot) {
    if (!space || !page_aligned(address) ||
        prot & ~(BOS_PROT_READ | BOS_PROT_WRITE | BOS_PROT_EXEC))
        return -BOS_EINVAL;
    length = (length + VM_PAGE - 1) & ~(VM_PAGE - 1);
    if (!range_valid(address, length)) return -BOS_EINVAL;
    uint32_t end = address + length;
    /* The whole range must be mapped. */
    for (uint32_t cursor = address; cursor < end;) {
        VmArea *area = area_at(space, cursor);
        if (!area) return -BOS_ENOMEM;
        cursor = area->end;
        if (!cursor) break;
    }
    if (split_at(space, address) || split_at(space, end)) return -BOS_ENOMEM;
    for (VmArea *area = space->areas; area && area->start < end; area = area->next)
        if (area->start >= address) area->prot = prot;
    /* PROT_NONE keeps the frame but makes it supervisor-only, so contents
     * survive a later mprotect back to an accessible protection. */
    uint32_t bits = (prot ? PTE_USER : 0) | (prot & BOS_PROT_WRITE ? PTE_WRITE : 0);
    for (uint32_t page = address; page < end; page += VM_PAGE) {
        if (!(directory_of(space)[page >> 22] & PTE_PRESENT)) continue;
        uint32_t *pte = pte_of(space, page, 0);
        if (!(*pte & PTE_PRESENT)) continue;
        *pte = (*pte & ~(PTE_USER | PTE_WRITE)) | bits;
        invalidate(space, page);
    }
    coalesce(space);
    return 0;
}

int vm_fault(VmSpace *space, uint32_t address, int write) {
    if (!space || address < BOS_USER_BASE || address >= BOS_USER_END) return 0;
    VmArea *area = area_at(space, address);
    if (!area || !(area->prot & (BOS_PROT_READ | BOS_PROT_WRITE | BOS_PROT_EXEC))) return 0;
    if (write && !(area->prot & BOS_PROT_WRITE)) return 0;
    uint32_t page = address & ~(VM_PAGE - 1);
    uint32_t *pte = pte_of(space, page, 1);
    if (!pte) return 0;
    if (*pte & PTE_PRESENT) {
        /* Present but faulted: a stale TLB entry after a protection change. */
        invalidate(space, page);
        return !write || (*pte & PTE_WRITE);
    }
    uint32_t frame;
    if (physmem_alloc_any(space->owner, PHYS_PROCESS_PAGE, 1, &frame, 0x100000000ull) != PHYS_OK)
        return 0;
    *pte = frame | PTE_PRESENT | PTE_USER | (area->prot & BOS_PROT_WRITE ? PTE_WRITE : 0);
    invalidate(space, page);
    ++space->resident_pages;
    return 1;
}

int vm_prepare(VmSpace *space, uint32_t address, uint32_t length, int write) {
    if (!space) return -BOS_EFAULT;
    if (!length) return 0;
    if (address < BOS_USER_BASE || address >= BOS_USER_END || length > BOS_USER_END - address)
        return -BOS_EFAULT;
    uint32_t end = address + length;
    for (uint32_t page = address & ~(VM_PAGE - 1); page < end; page += VM_PAGE) {
        VmArea *area = area_at(space, page);
        if (!area || !(area->prot & (BOS_PROT_READ | BOS_PROT_WRITE | BOS_PROT_EXEC)) ||
            (write && !(area->prot & BOS_PROT_WRITE)))
            return -BOS_EFAULT;
        uint32_t *pte = pte_of(space, page, 0);
        if (pte && (*pte & PTE_PRESENT)) continue;
        if (!vm_fault(space, page, write)) return -BOS_ENOMEM;
    }
    return 0;
}

int vm_string(VmSpace *space, uint32_t address, char *out, uint32_t capacity) {
    if (!space || !out || !capacity) return -BOS_EFAULT;
    /* The user window ends below 4 GiB, so page ends never wrap. */
    for (uint32_t length = 0;;) {
        if (vm_prepare(space, address, 1, 0)) return -BOS_EFAULT;
        uint32_t page_end = (address | (VM_PAGE - 1)) + 1;
        for (; address < page_end; ++address) {
            char value = *(const volatile char *)(uintptr_t)address;
            if (!value) {
                out[length] = 0;
                return (int)length;
            }
            if (length + 1 >= capacity) return -BOS_ENAMETOOLONG;
            out[length++] = value;
        }
    }
}

uint32_t vm_physical(VmSpace *space, uint32_t address) {
    if (!space || address < BOS_USER_BASE || address >= BOS_USER_END) return 0;
    if (!(directory_of(space)[address >> 22] & PTE_PRESENT)) return 0;
    uint32_t entry = *pte_of(space, address, 0);
    return entry & PTE_PRESENT ? (entry & ~(VM_PAGE - 1)) | (address & (VM_PAGE - 1)) : 0;
}
