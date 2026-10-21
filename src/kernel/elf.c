#include "kernel/elf.h"
#include "kernel/fs.h"

typedef struct {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version, entry, phoff, shoff, flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} ElfHeader;

typedef struct {
    uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
} ElfSegment;

#define PT_LOAD 1
#define PT_PHDR 6
#define PF_X 1
#define PF_W 2
#define PF_R 4
#define SEGMENT_MAX 64u

int elf_is_elf(const void *image, uint32_t size) {
    const uint8_t *bytes = image;
    return image && size >= 4 && bytes[0] == 0x7F && bytes[1] == 'E' && bytes[2] == 'L' && bytes[3] == 'F';
}

static unsigned prot_of(uint32_t flags) {
    return (flags & PF_R ? BOS_PROT_READ : 0) | (flags & PF_W ? BOS_PROT_WRITE : 0) |
           (flags & PF_X ? BOS_PROT_EXEC : 0);
}

int elf_load(VmSpace *space, const void *image, uint32_t size, ElfImage *out) {
    const uint8_t *bytes = image;
    ElfHeader header;
    if (!elf_is_elf(image, size) || size < sizeof header) return -BOS_EINVAL;
    kmemcpy(&header, bytes, sizeof header);
    if (header.ident[4] != 1 || header.ident[5] != 1 || header.ident[6] != 1 || header.type != 2 ||
        header.machine != 3 || header.phentsize != sizeof(ElfSegment) || !header.phnum ||
        header.phnum > SEGMENT_MAX || header.phoff > size ||
        (uint32_t)header.phnum * sizeof(ElfSegment) > size - header.phoff)
        return -BOS_EINVAL;
    ElfSegment segments[SEGMENT_MAX];
    kmemcpy(segments, bytes + header.phoff, header.phnum * sizeof(ElfSegment));
    unsigned loads = 0;
    uint32_t phdr = 0;
    /* Validate and reserve every segment read-write first. */
    for (unsigned i = 0; i < header.phnum; ++i) {
        const ElfSegment *s = &segments[i];
        if (s->type == PT_PHDR) phdr = s->vaddr;
        if (s->type != PT_LOAD || !s->memsz) continue;
        if (s->filesz > s->memsz || s->offset > size || s->filesz > size - s->offset ||
            (s->vaddr & (VM_PAGE - 1)) != (s->offset & (VM_PAGE - 1)) ||
            s->vaddr < BOS_USER_BASE || s->vaddr >= VM_MMAP_TOP || s->memsz > VM_MMAP_TOP - s->vaddr)
            return -BOS_EINVAL;
        uint32_t start = s->vaddr & ~(VM_PAGE - 1);
        uint32_t end = (s->vaddr + s->memsz + VM_PAGE - 1) & ~(VM_PAGE - 1);
        uint32_t mapped;
        int result = vm_map(space, start, end - start, BOS_PROT_READ | BOS_PROT_WRITE, 1, &mapped);
        if (result) return result;
        if (!phdr && header.phoff >= s->offset && header.phoff - s->offset < s->filesz)
            phdr = s->vaddr + (header.phoff - s->offset);
        ++loads;
    }
    if (!loads || header.entry < BOS_USER_BASE || header.entry >= VM_MMAP_TOP) return -BOS_EINVAL;
    /* Copy file bytes with the space active; the rest stays demand-zero. */
    int result = 0;
    vm_activate(space);
    for (unsigned i = 0; i < header.phnum && !result; ++i) {
        const ElfSegment *s = &segments[i];
        if (s->type != PT_LOAD || !s->filesz) continue;
        result = vm_prepare(space, s->vaddr, s->filesz, 1);
        if (!result) kmemcpy((void *)(uintptr_t)s->vaddr, bytes + s->offset, (int)s->filesz);
    }
    vm_activate(0);
    if (result) return result;
    /* Final protections; a page shared by two segments gets both. */
    for (unsigned i = 0; i < header.phnum; ++i) {
        const ElfSegment *s = &segments[i];
        if (s->type != PT_LOAD || !s->memsz) continue;
        uint32_t start = s->vaddr & ~(VM_PAGE - 1);
        uint32_t end = (s->vaddr + s->memsz + VM_PAGE - 1) & ~(VM_PAGE - 1);
        for (uint32_t page = start; page < end; page += VM_PAGE) {
            unsigned prot = 0;
            for (unsigned j = 0; j < header.phnum; ++j) {
                const ElfSegment *o = &segments[j];
                if (o->type != PT_LOAD || !o->memsz) continue;
                uint32_t o_start = o->vaddr & ~(VM_PAGE - 1);
                uint32_t o_end = (o->vaddr + o->memsz + VM_PAGE - 1) & ~(VM_PAGE - 1);
                if (page >= o_start && page < o_end) prot |= prot_of(o->flags);
            }
            /* Interior pages need one call per segment, not per page. */
            if (page != start && page + VM_PAGE < end) {
                result = vm_protect(space, page, end - VM_PAGE - page, prot_of(s->flags));
                if (result) return result;
                page = end - 2 * VM_PAGE;
                continue;
            }
            result = vm_protect(space, page, VM_PAGE, prot);
            if (result) return result;
        }
    }
    out->entry = header.entry;
    out->phdr = phdr;
    out->phent = header.phentsize;
    out->phnum = header.phnum;
    return 0;
}
