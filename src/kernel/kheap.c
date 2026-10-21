#include "kernel/kheap.h"
#include "kernel/physmem.h"
#include "kernel/platform.h"
#include "kernel/fs.h"

/* Size classes 16..2048 bytes. Each small page starts with a header and holds
 * objects of one class; free objects form a list inside the page. Pages are
 * linked per class while they have free space. Large objects are page runs
 * with a header page prefix sharing the same magic layout. */
#define CLASSES 8
#define SMALL_MAGIC 0x4B534D4Cu
#define LARGE_MAGIC 0x4B4C5247u
#define HEADER_BYTES 32u

typedef struct SmallPage {
    uint32_t magic, size, free_count, capacity;
    void *free_list;
    struct SmallPage *next, *prev;
    uint32_t in_list;
} SmallPage;
_Static_assert(sizeof(SmallPage) <= HEADER_BYTES, "small page header");

typedef struct {
    uint32_t magic, pages, bytes, reserved;
} LargeHeader;

static SmallPage *partial[CLASSES];
static KheapStats stats;

static unsigned class_of(uint32_t bytes) {
    unsigned index = 0;
    uint32_t size = 16;
    while (size < bytes) { size <<= 1; ++index; }
    return index;
}

static void list_remove(unsigned index, SmallPage *page) {
    if (page->prev) page->prev->next = page->next;
    else partial[index] = page->next;
    if (page->next) page->next->prev = page->prev;
    page->next = page->prev = 0;
    page->in_list = 0;
}

static void list_push(unsigned index, SmallPage *page) {
    page->prev = 0;
    page->next = partial[index];
    if (page->next) page->next->prev = page;
    partial[index] = page;
    page->in_list = 1;
}

static SmallPage *small_page_new(unsigned index) {
    uint32_t frame;
    if (physmem_alloc_any(PHYS_KERNEL_OWNER, PHYS_KERNEL_HEAP, 1, &frame, PHYS_KERNEL_LIMIT) != PHYS_OK)
        return 0;
    SmallPage *page = (SmallPage *)(uintptr_t)frame;
    uint32_t size = 16u << index;
    page->magic = SMALL_MAGIC;
    page->size = size;
    uint8_t *object = (uint8_t *)page + (size < HEADER_BYTES ? HEADER_BYTES : size);
    uint8_t *end = (uint8_t *)page + PHYS_PAGE_BYTES;
    void *head = 0;
    unsigned count = 0;
    /* Build the list so the lowest object is handed out first. */
    for (uint8_t *slot = end - size; slot >= object; slot -= size) {
        *(void **)slot = head;
        head = slot;
        ++count;
    }
    page->free_list = head;
    page->free_count = page->capacity = count;
    ++stats.pages;
    ++stats.small_pages;
    list_push(index, page);
    return page;
}

void *kmalloc(uint32_t bytes) {
    if (!bytes) bytes = 1;
    if (bytes <= 2048) {
        unsigned index = class_of(bytes);
        SmallPage *page = partial[index];
        if (!page && !(page = small_page_new(index))) return 0;
        void *object = page->free_list;
        page->free_list = *(void **)object;
        if (!--page->free_count) list_remove(index, page);
        kmemset(object, 0, (int)page->size);
        ++stats.live_objects;
        stats.live_bytes += page->size;
        return object;
    }
    if (bytes > 0x40000000u) return 0;
    uint32_t pages = (bytes + HEADER_BYTES + PHYS_PAGE_BYTES - 1) / PHYS_PAGE_BYTES, base;
    if (physmem_alloc_run(PHYS_KERNEL_OWNER, PHYS_KERNEL_HEAP, pages, &base, PHYS_KERNEL_LIMIT) != PHYS_OK)
        return 0;
    LargeHeader *header = (LargeHeader *)(uintptr_t)base;
    header->magic = LARGE_MAGIC;
    header->pages = pages;
    header->bytes = bytes;
    stats.pages += pages;
    ++stats.large_runs;
    ++stats.live_objects;
    stats.live_bytes += bytes;
    return (uint8_t *)header + HEADER_BYTES;
}

void kfree(void *pointer) {
    if (!pointer) return;
    uintptr_t address = (uintptr_t)pointer;
    if (address % PHYS_PAGE_BYTES == HEADER_BYTES) {
        LargeHeader *header = (LargeHeader *)(address - HEADER_BYTES);
        if (header->magic == LARGE_MAGIC) {
            uint32_t pages = header->pages;
            stats.pages -= pages;
            --stats.large_runs;
            --stats.live_objects;
            stats.live_bytes -= header->bytes;
            header->magic = 0;
            uint32_t frames[64];
            for (uint32_t done = 0; done < pages;) {
                unsigned batch = 0;
                while (batch < 64 && done < pages)
                    frames[batch++] = (uint32_t)(uintptr_t)header + done++ * PHYS_PAGE_BYTES;
                if (physmem_release(PHYS_KERNEL_OWNER, PHYS_KERNEL_HEAP, frames, batch) != PHYS_OK)
                    panic("kernel heap: large release");
            }
            return;
        }
    }
    SmallPage *page = (SmallPage *)(address & ~(uintptr_t)(PHYS_PAGE_BYTES - 1));
    if (page->magic != SMALL_MAGIC) panic("kernel heap: free of unknown pointer");
    uint32_t offset = (uint32_t)(address - (uintptr_t)page);
    uint32_t first = page->size < HEADER_BYTES ? HEADER_BYTES : page->size;
    if (offset < first || (offset - first) % page->size) panic("kernel heap: misaligned free");
    unsigned index = class_of(page->size);
    *(void **)pointer = page->free_list;
    page->free_list = pointer;
    ++page->free_count;
    --stats.live_objects;
    stats.live_bytes -= page->size;
    if (page->free_count == page->capacity && (page->in_list ? (page->next || page->prev) : 1)) {
        /* Keep one empty page per class; return the rest. */
        if (page->in_list) list_remove(index, page);
        page->magic = 0;
        uint32_t frame = (uint32_t)(uintptr_t)page;
        if (physmem_release(PHYS_KERNEL_OWNER, PHYS_KERNEL_HEAP, &frame, 1) != PHYS_OK)
            panic("kernel heap: page release");
        --stats.pages;
        --stats.small_pages;
        return;
    }
    if (!page->in_list) list_push(index, page);
}

void kheap_stats(KheapStats *out) {
    if (out) *out = stats;
}
