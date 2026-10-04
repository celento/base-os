#include "physmem.h"
#include "fs.h"

/* Transient claims are private while busy, never a persistent/public kind. */
#define CLAIM_BIT 0x80u
_Static_assert(PHYS_KIND_LIMIT < CLAIM_BIT, "page kinds overlap transient claim bit");

/* Source of truth: full static containers, including slack and dormant aliases.
 * Values come from layout.h, never a separately maintained list of free holes.
 * Owners: boot/linker; kernel graphics/editor; fs; writer/paint; term/gfx/download;
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
    X(compatibility_paging, PAGING_BASE, PAGING_CAPACITY) \
    X(whole_user_aperture_alias, USER_BASE, USER_APERTURE_CAPACITY) \
    X(drag_cache, DRAG_CACHE, DRAG_CAPACITY) \
    X(presentation, PRESENT_BASE, PRESENT_CAPACITY) \
    X(network_and_nic_dma, NET_BASE, NET_CAPACITY) \
    X(browser, BROWSER_BASE, BROWSER_CAPACITY) \
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
int physmem_core_init(PhysmemCore *core, uint32_t *owners, uint8_t *kinds,
                      const MemoryRange *map, unsigned count,
                      const BootInfo *video, PhysZeroPage zero_page, void *context) {
    if (!core || !owners || !kinds || !zero_page || !video || !video->lfb ||
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
    for (unsigned pfn = 0; pfn < PHYS_PAGE_COUNT; ++pfn) {
        uint32_t base = pfn * PHYS_PAGE_BYTES, end = base + PHYS_PAGE_BYTES;
        owners[pfn] = 0;
        kinds[pfn] = PHYS_UNAVAILABLE;
        if (statically_reserved(base, end) || (base < lfb_end && end > lfb_base) ||
            !platform_memory_map_available(map, count, base, end)) continue;
        kinds[pfn] = PHYS_FREE;
        ++stats.total;
    }
    stats.free = stats.total;
    core->owners = owners;
    core->kinds = kinds;
    core->zero_page = zero_page;
    core->zero_context = context;
    core->stats = stats;
    core->busy = 0;
    core->ready = 1;
    return PHYS_OK;
}
int physmem_core_alloc(PhysmemCore *core, BosHandle owner, enum PhysPageKind kind,
                       unsigned count, uint32_t *frames) {
    int status = core_status(core);
    if (status) return status;
    if (!owner_valid(owner) || !kind_valid(kind) || !frames || !count || count > PHYS_BATCH_MAX)
        return PHYS_INVALID;
    if (count > core->stats.free) return PHYS_CAPACITY;
    /* Preflight metadata as well as accounting. Serialized context and an
     * infallible zero callback make the following claim/clear pass atomic to
     * all allocator clients without disabling interrupts across zeroing. */
    unsigned available = 0, last = 0;
    for (unsigned pfn = 0; pfn < PHYS_PAGE_COUNT && available < count; ++pfn)
        if (core->kinds[pfn] == PHYS_FREE) { ++available; last = pfn; }
    if (available != count) return PHYS_CAPACITY;
    core->busy = 1;
    for (unsigned pfn = 0; pfn <= last; ++pfn) {
        if (core->kinds[pfn] != PHYS_FREE) continue;
        core->owners[pfn] = owner;
        core->kinds[pfn] = kind | CLAIM_BIT;
        core->zero_page(pfn * PHYS_PAGE_BYTES, core->zero_context);
    }
    /* Publish only after all selected pages are cleared. An owner can have
     * existing pages of the same kind, so select the just-claimed set using
     * the original free-page positions retained in an internal transient bit. */
    unsigned output = 0;
    for (unsigned pfn = 0; pfn <= last; ++pfn)
        if (core->kinds[pfn] == (kind | CLAIM_BIT)) {
            frames[output++] = pfn * PHYS_PAGE_BYTES;
            core->kinds[pfn] = kind;
        }
    core->stats.free -= count;
    core->stats.allocated += count;
    core->stats.by_kind[kind] += count;
    if (core->stats.allocated > core->stats.high_water) core->stats.high_water = core->stats.allocated;
    core->busy = 0;
    return PHYS_OK;
}
int physmem_core_release(PhysmemCore *core, BosHandle owner, enum PhysPageKind kind,
                         const uint32_t *frames, unsigned count) {
    int status = core_status(core);
    if (status) return status;
    if (!owner_valid(owner) || !kind_valid(kind) || !frames || !count || count > PHYS_BATCH_MAX)
        return PHYS_INVALID;
    for (unsigned i = 0; i < count; ++i) {
        uint32_t frame = frames[i];
        if (frame % PHYS_PAGE_BYTES || frame >= PHYS_MANAGED_END) return PHYS_INVALID;
        unsigned pfn = frame / PHYS_PAGE_BYTES;
        if (core->owners[pfn] != owner || core->kinds[pfn] != kind) return PHYS_NOT_OWNED;
        /* Bounded by PHYS_BATCH_MAX; no temporary metadata writes or rollback. */
        for (unsigned j = 0; j < i; ++j) if (frames[j] == frame) return PHYS_INVALID;
    }
    for (unsigned i = 0; i < count; ++i) {
        unsigned pfn = frames[i] / PHYS_PAGE_BYTES;
        core->owners[pfn] = 0;
        core->kinds[pfn] = PHYS_FREE;
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
    for (unsigned pfn = 0; pfn < PHYS_PAGE_COUNT; ++pfn) {
        if (core->owners[pfn] != owner) continue;
        --core->stats.by_kind[core->kinds[pfn]];
        --core->stats.allocated;
        ++core->stats.free;
        core->owners[pfn] = 0;
        core->kinds[pfn] = PHYS_FREE;
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
    for (unsigned pfn = 0; pfn < PHYS_PAGE_COUNT; ++pfn) count += core->owners[pfn] == owner;
    return count;
}

static PhysmemCore physical;
static void zero_physical_page(uint32_t frame, void *context) {
    (void)context;
    kmemset((void *)(uintptr_t)frame, 0, PHYS_PAGE_BYTES);
}
int physmem_init(void) {
    const BootInfo *boot = (const BootInfo *)BOOTINFO_ADDR;
    /* Do not touch the metadata arena before the normal kmain admission gate.
     * Independently verify the exact destination before its first write. */
    if (!video_info_valid(boot) ||
        !platform_memory_range_available(TASK_PAGE_METADATA_BASE,
                                        TASK_PAGE_METADATA_BASE + TASK_PAGE_METADATA_CAPACITY))
        return PHYS_INVALID;
    return physmem_core_init(&physical, (uint32_t *)TASK_PAGE_METADATA_BASE,
        (uint8_t *)(TASK_PAGE_METADATA_BASE + PHYS_PAGE_COUNT * sizeof(uint32_t)),
        (const MemoryRange *)E820_BASE, boot->map_count, boot, zero_physical_page, 0);
}
int physmem_alloc(BosHandle owner, enum PhysPageKind kind, unsigned count, uint32_t *frames) {
    return physmem_core_alloc(&physical, owner, kind, count, frames);
}
int physmem_release(BosHandle owner, enum PhysPageKind kind, const uint32_t *frames, unsigned count) {
    return physmem_core_release(&physical, owner, kind, frames, count);
}
int physmem_release_owner(BosHandle owner) { return physmem_core_release_owner(&physical, owner); }
int physmem_stats(PhysmemStats *stats) { return physmem_core_stats(&physical, stats); }
unsigned physmem_owner_pages(BosHandle owner) { return physmem_core_owner_pages(&physical, owner); }
