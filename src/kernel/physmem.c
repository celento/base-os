#include "kernel/physmem.h"
#include "kernel/fs.h"

/* Transient claims are private while busy, never a persistent/public kind. */
#define CLAIM_BIT 0x80u
_Static_assert(PHYS_KIND_LIMIT < CLAIM_BIT, "page kinds overlap transient claim bit");

/* Source of truth: full static containers, including slack and dormant aliases.
 * Values come from layout.h, never a separately maintained list of free holes.
 * Owners: boot/linker (including kernel BSS in the reused staging area); kernel graphics/editor; fs; writer/paint; term/gfx/download;
 * persist/audio/sheet; process paging/tasks; net (including RTL8139 DMA);
 * browser/image_viewer/video. Both filesystem profiles and both audio workspaces
 * stay reserved regardless of mount/fallback/migration/playback state. */
#define STATIC_RESERVATIONS(X) \
    X(low_boot_kernel_stack, 0, FB_BASE) \
    X(software_framebuffer, FB_BASE, FB_CAPACITY) \
    X(filesystem_nodes, FS_BASE, FS_CAPACITY) \
    X(writer, WRITER_BASE, WRITER_CAPACITY) \
    X(paint, PAINT_MEM, PAINT_CAPACITY) \
    X(native_published_canvas, NATIVE_CANVAS_BASE, NATIVE_CANVAS_CAPACITY) \
    X(floppy_dma, DMA_BASE, DMA_CAPACITY) \
    X(audio_dma, AUDIO_DMA_BASE, AUDIO_DMA_CAPACITY) \
    X(sheet, SHEET_BASE, SHEET_CAPACITY) \
    X(desktop_cache, DESK_CACHE, DESK_CAPACITY) \
    X(apps_including_subarenas_and_slack, APPS_BASE, APPS_CAPACITY) \
    X(native_paging_roots, PAGING_BASE, PAGING_CAPACITY) \
    X(whole_user_aperture_alias, USER_BASE, USER_APERTURE_CAPACITY) \
    X(drag_cache, DRAG_CACHE, DRAG_CAPACITY) \
    X(presentation, PRESENT_BASE, PRESENT_CAPACITY) \
    X(network_and_nic_dma, NET_BASE, NET_CAPACITY) \
    X(browser, BROWSER_BASE, BROWSER_CAPACITY) \
    X(ui_icons, ICON_BASE, ICON_CAPACITY) \
    X(default_audio_workspace, AUDIO_WORK_BASE, AUDIO_WORK_CAPACITY) \
    X(image_viewer_and_decoder, IMAGE_BASE, IMAGE_CAPACITY) \
    X(default_fs_pool, FS_POOL_BASE, FS_POOL_CAPACITY) \
    X(default_fs_staging, FS_IMG_BASE, FS_IMG_CAPACITY) \
    X(large_audio_alias, AUDIO_LARGE_WORK_BASE, AUDIO_LARGE_WORK_CAPACITY) \
    X(tasks_metadata_and_syscall_stack, TASK_BASE, TASK_CAPACITY) \
    X(editor_windows_clipboard, EDITOR_BASE, EDITOR_CAPACITY) \
    X(video, VIDEO_BASE, VIDEO_CAPACITY) \
    X(large_fs_pool_all_profiles, FS_LARGE_POOL_BASE, FS_LARGE_POOL_CAPACITY) \
    X(large_fs_staging_all_profiles, FS_LARGE_IMG_BASE, FS_LARGE_IMG_CAPACITY)

#define RESERVATION(name, base, capacity) {#name, base, (base) + (capacity)},
static const PhysReservation reservations[] = { STATIC_RESERVATIONS(RESERVATION) };
#undef RESERVATION
#define CHECK_RESERVATION(name, base, capacity) \
    _Static_assert((uint64_t)(base) + (capacity) <= PHYS_MANAGED_END && \
                   (capacity) > 0, "invalid static reservation: " #name);
STATIC_RESERVATIONS(CHECK_RESERVATION)
#undef CHECK_RESERVATION

_Static_assert(PHYS_PAGE_BYTES == 4096 && PHYS_MANAGED_END == 0x10000000 &&
               PHYS_PAGE_COUNT == 65536, "review owned-page ceiling before changing");
_Static_assert(TASK_PAGE_METADATA_BASE % PHYS_PAGE_BYTES == 0 &&
               TASK_PAGE_METADATA_BASE >= TASK_BASE &&
               TASK_PAGE_METADATA_BASE + TASK_PAGE_METADATA_CAPACITY <= TASK_INTERRUPT_STACK_BASE,
               "page metadata overlaps task bounds/syscall stack");
_Static_assert(PHYS_PAGE_COUNT * (sizeof(uint32_t) + sizeof(uint8_t)) == TASK_PAGE_METADATA_CAPACITY,
               "page metadata arrays must fit exact reservation");
_Static_assert(TASK_PAGE_METADATA_BASE + TASK_PAGE_METADATA_CAPACITY <= TASK_BASE + TASK_CAPACITY &&
               TASK_BASE + TASK_CAPACITY <= RAM_REQUIRED_END, "metadata outside validated RAM");
_Static_assert(USER_BASE % 0x400000 == 0 && USER_APERTURE_CAPACITY == 0x400000 &&
               USER_BASE + USER_APERTURE_CAPACITY <= DRAG_CACHE,
               "exclude the entire non-identity compatibility PDE");

const PhysReservation *physmem_reservations(unsigned *count) {
    if (count) *count = sizeof reservations / sizeof reservations[0];
    return reservations;
}
static int statically_reserved(uint32_t base, uint32_t end) {
    for (unsigned i = 0; i < sizeof reservations / sizeof reservations[0]; ++i)
        if (reservations[i].base < end && reservations[i].end > base) return 1;
    return 0;
}
static int owner_valid(BosHandle owner) {
    return (owner & ~BOS_HANDLE_SERIAL_MAX) == BOS_HANDLE_TYPE_PROCESS &&
           (owner & BOS_HANDLE_SERIAL_MAX) != 0;
}
static int kind_valid(enum PhysPageKind kind) {
    return kind >= PHYS_BEX1_BACKING && kind < PHYS_KIND_LIMIT;
}
static int core_status(const PhysmemCore *core) {
    if (!core || !core->ready) return PHYS_NOT_READY;
    return core->busy ? PHYS_BUSY : PHYS_OK;
}
int physmem_core_init_range(PhysmemCore *core, uint32_t *owners, uint8_t *kinds,
                            unsigned first_pfn, unsigned page_count,
                            const MemoryRange *map, unsigned count,
                            const BootInfo *video, PhysZeroPage zero_page, void *context) {
    if (!core || !owners || !kinds || !zero_page || !video || !video->lfb || !page_count ||
        (uint64_t)first_pfn + page_count > 0x100000ULL ||
        !platform_memory_map_valid(map, count)) return PHYS_INVALID;
    if (core->ready || core->busy) return PHYS_BUSY;
    /* Widen before multiplication/addition; round outwards for all supported
     * runtime 32-bit modes, not just the selected boot mode. End may be 4 GiB. */
    uint64_t span = (uint64_t)video->pitch * video->height;
    if (span < 4ULL * FB_CAPACITY) span = 4ULL * FB_CAPACITY;
    uint64_t lfb_end = (uint64_t)video->lfb + span;
    if (lfb_end > 0x100000000ULL) return PHYS_INVALID;
    uint64_t lfb_base = video->lfb & ~(uint64_t)(PHYS_PAGE_BYTES - 1);
    lfb_end = (lfb_end + PHYS_PAGE_BYTES - 1) & ~(uint64_t)(PHYS_PAGE_BYTES - 1);
    PhysmemStats stats = {0};
    for (unsigned index = 0; index < page_count; ++index) {
        uint64_t base = (uint64_t)(first_pfn + index) * PHYS_PAGE_BYTES, end = base + PHYS_PAGE_BYTES;
        owners[index] = 0;
        kinds[index] = PHYS_UNAVAILABLE;
        if ((end <= PHYS_MANAGED_END && statically_reserved((uint32_t)base, (uint32_t)end)) ||
            (base < lfb_end && end > lfb_base) ||
            !platform_memory_map_available(map, count, base, end)) continue;
        kinds[index] = PHYS_FREE;
        ++stats.total;
    }
    stats.free = stats.total;
    core->owners = owners;
    core->kinds = kinds;
    core->zero_page = zero_page;
    core->zero_context = context;
    core->stats = stats;
    core->first = first_pfn;
    core->count = page_count;
    core->hint = 0;
    core->busy = 0;
    core->ready = 1;
    return PHYS_OK;
}
int physmem_core_init(PhysmemCore *core, uint32_t *owners, uint8_t *kinds,
                      const MemoryRange *map, unsigned count,
                      const BootInfo *video, PhysZeroPage zero_page, void *context) {
    return physmem_core_init_range(core, owners, kinds, 0, PHYS_PAGE_COUNT, map, count,
                                   video, zero_page, context);
}
/* Index bound for a byte limit, clipped to the core. */
static unsigned limit_index(const PhysmemCore *core, uint64_t limit) {
    uint64_t pfn = limit / PHYS_PAGE_BYTES;
    if (pfn <= core->first) return 0;
    pfn -= core->first;
    return pfn < core->count ? (unsigned)pfn : core->count;
}
static void advance_hint(PhysmemCore *core) {
    while (core->hint < core->count && core->kinds[core->hint] != PHYS_FREE) ++core->hint;
}
static void account_alloc(PhysmemCore *core, enum PhysPageKind kind, unsigned count) {
    core->stats.free -= count;
    core->stats.allocated += count;
    core->stats.by_kind[kind] += count;
    if (core->stats.allocated > core->stats.high_water) core->stats.high_water = core->stats.allocated;
}
int physmem_core_alloc_below(PhysmemCore *core, BosHandle owner, enum PhysPageKind kind,
                             unsigned count, uint32_t *frames, uint64_t limit) {
    int status = core_status(core);
    if (status) return status;
    if (!owner_valid(owner) || !kind_valid(kind) || !frames || !count || count > PHYS_BATCH_MAX)
        return PHYS_INVALID;
    if (count > core->stats.free) return PHYS_CAPACITY;
    /* Preflight metadata as well as accounting. Serialized context and an
     * infallible zero callback make the following claim/clear pass atomic to
     * all allocator clients without disabling interrupts across zeroing. */
    advance_hint(core);
    unsigned end = limit_index(core, limit), available = 0, last = 0;
    for (unsigned index = core->hint; index < end && available < count; ++index)
        if (core->kinds[index] == PHYS_FREE) { ++available; last = index; }
    if (available != count) return PHYS_CAPACITY;
    core->busy = 1;
    for (unsigned index = core->hint; index <= last; ++index) {
        if (core->kinds[index] != PHYS_FREE) continue;
        core->owners[index] = owner;
        core->kinds[index] = kind | CLAIM_BIT;
        core->zero_page((core->first + index) * PHYS_PAGE_BYTES, core->zero_context);
    }
    /* Publish only after all selected pages are cleared. An owner can have
     * existing pages of the same kind, so select the just-claimed set using
     * the original free-page positions retained in an internal transient bit. */
    unsigned output = 0;
    for (unsigned index = core->hint; index <= last; ++index)
        if (core->kinds[index] == (kind | CLAIM_BIT)) {
            frames[output++] = (core->first + index) * PHYS_PAGE_BYTES;
            core->kinds[index] = kind;
        }
    account_alloc(core, kind, count);
    core->busy = 0;
    return PHYS_OK;
}
int physmem_core_alloc(PhysmemCore *core, BosHandle owner, enum PhysPageKind kind,
                       unsigned count, uint32_t *frames) {
    return physmem_core_alloc_below(core, owner, kind, count, frames, 0x100000000ULL);
}
int physmem_core_alloc_run(PhysmemCore *core, BosHandle owner, enum PhysPageKind kind,
                           unsigned count, uint32_t *base, uint64_t limit) {
    int status = core_status(core);
    if (status) return status;
    if (!owner_valid(owner) || !kind_valid(kind) || !base || !count) return PHYS_INVALID;
    if (count > core->stats.free) return PHYS_CAPACITY;
    advance_hint(core);
    unsigned end = limit_index(core, limit), run = 0, first = 0;
    for (unsigned index = core->hint; index < end && run < count; ++index) {
        if (core->kinds[index] != PHYS_FREE) { run = 0; continue; }
        if (!run++) first = index;
    }
    if (run != count) return PHYS_CAPACITY;
    core->busy = 1;
    for (unsigned index = first; index < first + count; ++index) {
        core->owners[index] = owner;
        core->kinds[index] = kind;
        core->zero_page((core->first + index) * PHYS_PAGE_BYTES, core->zero_context);
    }
    account_alloc(core, kind, count);
    core->busy = 0;
    *base = (core->first + first) * PHYS_PAGE_BYTES;
    return PHYS_OK;
}
static int frame_index(const PhysmemCore *core, uint32_t frame, unsigned *index) {
    if (frame % PHYS_PAGE_BYTES) return 0;
    unsigned pfn = frame / PHYS_PAGE_BYTES;
    if (pfn < core->first || pfn - core->first >= core->count) return 0;
    *index = pfn - core->first;
    return 1;
}
int physmem_core_release(PhysmemCore *core, BosHandle owner, enum PhysPageKind kind,
                         const uint32_t *frames, unsigned count) {
    int status = core_status(core);
    if (status) return status;
    if (!owner_valid(owner) || !kind_valid(kind) || !frames || !count || count > PHYS_BATCH_MAX)
        return PHYS_INVALID;
    for (unsigned i = 0; i < count; ++i) {
        uint32_t frame = frames[i];
        unsigned index;
        if (!frame_index(core, frame, &index)) return PHYS_INVALID;
        if (core->owners[index] != owner || core->kinds[index] != kind) return PHYS_NOT_OWNED;
        /* Bounded by PHYS_BATCH_MAX; no temporary metadata writes or rollback. */
        for (unsigned j = 0; j < i; ++j) if (frames[j] == frame) return PHYS_INVALID;
    }
    for (unsigned i = 0; i < count; ++i) {
        unsigned index = 0;
        frame_index(core, frames[i], &index);
        core->owners[index] = 0;
        core->kinds[index] = PHYS_FREE;
        if (index < core->hint) core->hint = index;
    }
    core->stats.free += count;
    core->stats.allocated -= count;
    core->stats.by_kind[kind] -= count;
    return PHYS_OK;
}
int physmem_core_release_owner(PhysmemCore *core, BosHandle owner) {
    int status = core_status(core);
    if (status) return status;
    if (!owner_valid(owner)) return PHYS_INVALID;
    for (unsigned index = 0; index < core->count; ++index) {
        if (core->owners[index] != owner) continue;
        --core->stats.by_kind[core->kinds[index]];
        --core->stats.allocated;
        ++core->stats.free;
        core->owners[index] = 0;
        core->kinds[index] = PHYS_FREE;
        if (index < core->hint) core->hint = index;
    }
    return PHYS_OK;
}
int physmem_core_stats(const PhysmemCore *core, PhysmemStats *stats) {
    int status = core_status(core);
    if (status) return status;
    if (!stats) return PHYS_INVALID;
    *stats = core->stats;
    return PHYS_OK;
}
unsigned physmem_core_owner_pages(const PhysmemCore *core, BosHandle owner) {
    if (core_status(core) || !owner_valid(owner)) return 0;
    unsigned count = 0;
    for (unsigned index = 0; index < core->count; ++index) count += core->owners[index] == owner;
    return count;
}

static PhysmemCore physical, high;
/* Frames inside the ELF user window are not identity-mapped while a process
 * root is loaded, so clear them through the kernel root. */
static void zero_physical_page(uint32_t frame, void *context) {
    (void)context;
    if (frame < PHYS_KERNEL_LIMIT || frame >= 0xC0000000u) {
        kmemset((void *)(uintptr_t)frame, 0, PHYS_PAGE_BYTES);
        return;
    }
#if defined(__i386__)
    unsigned flags, root;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(flags) :: "memory");
    __asm__ volatile("mov %%cr3,%0" : "=r"(root));
    __asm__ volatile("mov %0,%%cr3" :: "r"(KERNEL_DIRECTORY_BASE) : "memory");
    kmemset((void *)(uintptr_t)frame, 0, PHYS_PAGE_BYTES);
    __asm__ volatile("mov %0,%%cr3" :: "r"(root) : "memory");
    __asm__ volatile("pushl %0; popfl" :: "r"(flags) : "memory", "cc");
#else
    /* Host builds of the allocator never reach the high zone. */
    kmemset((void *)(uintptr_t)frame, 0, PHYS_PAGE_BYTES);
#endif
}
/* Highest usable E820 byte below 4 GiB. */
static uint64_t ram_top(const MemoryRange *map, unsigned count) {
    uint64_t top = 0;
    for (unsigned i = 0; i < count; ++i) {
        const MemoryRange *r = &map[i];
        if (r->type != 1 || !(r->attributes & 1) || !r->length) continue;
        uint64_t end = r->base + r->length;
        if (end > 0x100000000ULL) end = 0x100000000ULL;
        if (end > top) top = end;
    }
    return top & ~(uint64_t)(PHYS_PAGE_BYTES - 1);
}
/* RAM above the compatibility pool. Its metadata (five bytes per frame) takes
 * the first usable run below PHYS_KERNEL_LIMIT, which the zone then excludes. */
static void high_init(const MemoryRange *map, unsigned count, const BootInfo *boot) {
    uint64_t top = ram_top(map, count);
    if (top <= PHYS_MANAGED_END) return;
    unsigned first = PHYS_MANAGED_END / PHYS_PAGE_BYTES;
    unsigned pages = (unsigned)(top / PHYS_PAGE_BYTES) - first;
    uint32_t bytes = pages * 5u;
    uint32_t metadata = (bytes + PHYS_PAGE_BYTES - 1) & ~(PHYS_PAGE_BYTES - 1);
    for (uint64_t base = PHYS_MANAGED_END; base + metadata <= top && base + metadata <= PHYS_KERNEL_LIMIT;
         base += PHYS_PAGE_BYTES) {
        if (!platform_memory_map_available(map, count, base, base + metadata)) continue;
        uint32_t *owners = (uint32_t *)(uintptr_t)base;
        uint8_t *kinds = (uint8_t *)(uintptr_t)(base + pages * 4u);
        if (physmem_core_init_range(&high, owners, kinds, first, pages, map, count, boot,
                                    zero_physical_page, 0) != PHYS_OK) return;
        unsigned used = metadata / PHYS_PAGE_BYTES, start = (unsigned)(base / PHYS_PAGE_BYTES) - first;
        for (unsigned index = start; index < start + used; ++index) {
            if (high.kinds[index] != PHYS_FREE) continue;
            high.kinds[index] = PHYS_UNAVAILABLE;
            --high.stats.total;
            --high.stats.free;
        }
        return;
    }
}
int physmem_init(void) {
    const BootInfo *boot = (const BootInfo *)BOOTINFO_ADDR;
    /* Do not touch the metadata arena before the normal kmain admission gate.
     * Independently verify the exact destination before its first write. */
    if (!video_info_valid(boot) ||
        !platform_memory_range_available(TASK_PAGE_METADATA_BASE,
                                        TASK_PAGE_METADATA_BASE + TASK_PAGE_METADATA_CAPACITY))
        return PHYS_INVALID;
    int result = physmem_core_init(&physical, (uint32_t *)TASK_PAGE_METADATA_BASE,
        (uint8_t *)(TASK_PAGE_METADATA_BASE + PHYS_PAGE_COUNT * sizeof(uint32_t)),
        (const MemoryRange *)E820_BASE, boot->map_count, boot, zero_physical_page, 0);
    if (result == PHYS_OK) high_init((const MemoryRange *)E820_BASE, boot->map_count, boot);
    return result;
}
int physmem_alloc(BosHandle owner, enum PhysPageKind kind, unsigned count, uint32_t *frames) {
    return physmem_core_alloc(&physical, owner, kind, count, frames);
}
static PhysmemCore *frame_core(uint32_t frame) {
    return frame >= PHYS_MANAGED_END && high.ready ? &high : &physical;
}
int physmem_release(BosHandle owner, enum PhysPageKind kind, const uint32_t *frames, unsigned count) {
    if (!frames || !count) return physmem_core_release(&physical, owner, kind, frames, count);
    /* Batches never mix zones: release each zone's frames separately. */
    PhysmemCore *core = frame_core(frames[0]);
    for (unsigned i = 1; i < count; ++i)
        if (frame_core(frames[i]) != core) {
            int result = physmem_release(owner, kind, frames, i);
            return result ? result : physmem_release(owner, kind, frames + i, count - i);
        }
    return physmem_core_release(core, owner, kind, frames, count);
}
int physmem_release_owner(BosHandle owner) {
    int result = physmem_core_release_owner(&physical, owner);
    if (result == PHYS_OK && high.ready) result = physmem_core_release_owner(&high, owner);
    return result;
}
int physmem_stats(PhysmemStats *stats) { return physmem_core_stats(&physical, stats); }
unsigned physmem_owner_pages(BosHandle owner) {
    return physmem_core_owner_pages(&physical, owner) +
           (high.ready ? physmem_core_owner_pages(&high, owner) : 0);
}
int physmem_alloc_any(BosHandle owner, enum PhysPageKind kind, unsigned count,
                      uint32_t *frames, uint64_t limit) {
    /* All from the high zone, else all from the pool: a batch stays in one
     * zone, so a caller's rollback is one release call. */
    if (high.ready && physmem_core_alloc_below(&high, owner, kind, count, frames, limit) == PHYS_OK)
        return PHYS_OK;
    return physmem_core_alloc_below(&physical, owner, kind, count, frames, limit);
}
int physmem_alloc_run(BosHandle owner, enum PhysPageKind kind, unsigned count,
                      uint32_t *base, uint64_t limit) {
    if (high.ready && physmem_core_alloc_run(&high, owner, kind, count, base, limit) == PHYS_OK)
        return PHYS_OK;
    return physmem_core_alloc_run(&physical, owner, kind, count, base, limit);
}
int physmem_stats_all(PhysmemStats *stats) {
    int result = physmem_core_stats(&physical, stats);
    PhysmemStats extra;
    if (result || !high.ready || physmem_core_stats(&high, &extra)) return result;
    stats->total += extra.total;
    stats->free += extra.free;
    stats->allocated += extra.allocated;
    stats->high_water += extra.high_water;
    for (unsigned kind = 0; kind < PHYS_KIND_LIMIT; ++kind) stats->by_kind[kind] += extra.by_kind[kind];
    return PHYS_OK;
}
