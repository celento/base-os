#ifndef LAYOUT_H
#define LAYOUT_H
/* Shared by C, the preprocessed linker script, assembler and image tools.
 * Keep these values integer literals so tools/layout.py can read them. */
/* BIOS reads below conventional RAM; the protected kernel runs above 1 MiB. */
#define KERNEL_STAGE_ADDR 0x10000
#define KERNEL_STAGE_LIMIT 0x90000
#define KERNEL_LOAD_ADDR 0x100000
#define KERNEL_BOOTSTRAP_BYTES 4096
#define KERNEL_PACK_HEADER_BYTES 32
#define KERNEL_PACK_MAGIC 0x314B5042
#define KERNEL_BOOT_STACK_BOTTOM 0x9000
#define KERNEL_BOOT_STACK_TOP 0x10000
#define KERNEL_SECTORS 959
#define KERNEL_PRIMARY_SECTORS 383
#define KERNEL_EXT_LBA 5184
#define STACK_BOTTOM 0x1F0000
#define STACK_TOP 0x200000
#define BOOTINFO_ADDR 0x7E00
#define BOOTINFO_MAGIC 0x72072072
#define E820_BASE 0x5000
#define E820_MAX 128
#define FB_BASE 0x200000
#define FB_CAPACITY 0x100000
#define FS_LEGACY_NODES 64
#define FS_MAX_NODES 256
#define FS_MAX_DEPTH 63
#define FS_PATH_LEN 1536
#define FS_BASE 0x300000
#define FS_CAPACITY 0x10000
#define WRITER_BASE 0x310000
#define WRITER_CAPACITY 0x1F0000
#define PAINT_MEM 0x500000
#define PAINT_CAPACITY 0x10000
/* Last complete native frames, separate from per-Terminal working pixels. */
#define NATIVE_CANVAS_BASE 0x600000
#define NATIVE_CANVAS_CAPACITY 0x80000
#define DMA_BASE 0x700000
#define DMA_CAPACITY 0x10000
#define AUDIO_DMA_BASE 0x710000
#define AUDIO_DMA_CAPACITY 0x10000
#define SHEET_BASE 0x720000
#define SHEET_CAPACITY 0x2E0000
#define AUDIO_WORK_BASE 0x1700000
#define AUDIO_WORK_CAPACITY 0x200000
/* Reuses legacy FS pool/staging only after a high-arena large-volume mount. */
#define AUDIO_LARGE_WORK_BASE 0x2000000
#define AUDIO_LARGE_WORK_CAPACITY 0x1000000
#define FS_IMG_BASE 0x2800000
#define FS_IMG_CAPACITY 0x800000
#define DESK_CACHE 0xA00000
#define DESK_CAPACITY 0x100000
#define APPS_BASE 0xB00000
#define APPS_CAPACITY 0x400000
#define PAGING_BASE 0xF00000
#define PAGING_CAPACITY 0x3000
/* Separate inactive kernel root. Both compatibility pages retain their layout. */
#define KERNEL_DIRECTORY_BASE 0xF02000
#define USER_BASE 0x1000000
#define USER_CAPACITY 0x10000
/* The compatibility page directory replaces this entire 4 MiB PDE. Unused
 * addresses are not kernel identity mappings, so exclude every physical alias. */
#define USER_APERTURE_CAPACITY 0x400000
#define DRAG_CACHE 0x1400000
#define DRAG_CAPACITY 0x100000
#define PRESENT_BASE 0x1500000
#define PRESENT_CAPACITY 0x100000
#define NET_BASE 0x1600000
#define NET_CAPACITY 0x10000
#define BROWSER_BASE 0x1610000
#define BROWSER_CAPACITY 0x40000
#define PAINT_HISTORY_BASE 0xB00000
#define PAINT_HISTORY_CAPACITY 0x20000
#define DOWNLOAD_BASE 0xB30000
#define DOWNLOAD_CAPACITY 0x210000
#define GFX_CACHE_BASE 0xDF0000
#define GFX_CACHE_CAPACITY 0x10000
#define FS_POOL_BASE 0x2000000
#define FS_POOL_CAPACITY 0x800000
#define IMAGE_BASE 0x1900000
#define IMAGE_CAPACITY 0x700000
#define TASK_BASE 0x3000000
#define TASK_CAPACITY 0x100000
/* Owned-page metadata: 65,536 aligned uint32 owners, then one-byte kinds.
 * Task records must end before this boundary even while images remain inline. */
#define TASK_PAGE_METADATA_BASE 0x3090000
#define TASK_PAGE_METADATA_CAPACITY 0x50000
#define TASK_INTERRUPT_STACK_BASE 0x30E0000
#define TASK_INTERRUPT_STACK_CAPACITY 0x10000
#define EDITOR_BASE 0x3100000
#define EDITOR_CAPACITY 0x500000
#define VIDEO_BASE 0x3600000
#define VIDEO_CAPACITY 0x900000
#define RAM_REQUIRED_END 0x3F00000
/* Optional large-volume arenas. Selected only after a complete E820 check. */
#define FS_LARGE_POOL_BASE 0x3F00000
#define FS_LARGE_POOL_CAPACITY 0x2000000
#define FS_LARGE_IMG_BASE 0x5F00000
#define FS_LARGE_IMG_CAPACITY 0x2000000
#define RAM_LARGE_REQUIRED_END 0x7F00000
/* Explicit first allocator ceiling; never infer this from a RAM-size label. */
#define PHYS_PAGE_BYTES 4096
#define PHYS_MANAGED_END 0x10000000
#define FS_DISK_LBA 384
#define FS_DISK_SECTORS 2400
#define FS_SECOND_LBA 2784
#define DISK_SECTORS 5760
#define DATA_DISK_SECTORS 32768
#define DATA_SLOT_SECTORS 16383
#define DATA_FIRST_LBA 1
#define DATA_SECOND_LBA 16384
#define DATA_MARKER_MAGIC 0x44534F42
#define DATA_MARKER_VERSION 1
/* Distinct, explicitly initialized geometry; never resize an existing disk. */
#define DATA_LARGE_DISK_SECTORS 131072
#define DATA_LARGE_SLOT_SECTORS 65535
#define DATA_LARGE_FIRST_LBA 1
#define DATA_LARGE_SECOND_LBA 65536
#define DATA_LARGE_MARKER_VERSION 2
#define SECTOR_SIZE 512
#endif
