# Technical reference

[Back to BaseOS](../README.md) · [Desktop and programming guide](../FEATURES.md)

## Build and data safety

Normal builds update only the boot/kernel reservation. Filesystem sectors survive rebuilds and `make clean`. Before updating an existing image, the build saves its previous contents as `build/baseos.img.<hash>.bak`. The updater refuses an image locked by a running QEMU guest; shut the guest down before rebuilding.

The image updater extends an old 1.44 MB image to 2.88 MB without changing its existing volume bytes or LBAs. The kernel reads v1 and v2 filesystems and writes v3, which adds modification timestamps. Its first changed save goes into the second snapshot, leaving the old snapshot intact. Each floppy snapshot retains the full existing capacity: 64 nodes with up to 16,383 data bytes per file.

Saves alternate between two snapshots. A save writes and verifies the new payload, then commits a checksummed header. On boot, the kernel validates both snapshots and loads the newest complete one. It never automatically reformats a corrupt or unreadable disk. The desktop displays a disk warning if changes are only in RAM; Shutdown stays in the desktop if flushing fails. Automatic retries are five seconds apart and stop after three failures. Selecting Shutdown explicitly retries a failed flush, unless a mount or uncertain commit has protected the disk until reboot.

The current split-extent loader requires a 2.88 MB floppy. The image updater upgrades legacy 1.44 MB disks before installing this loader, preserving both the existing volume data and an original-image backup. Keep backups outside the running image for host disk failure or damage to both snapshots. The snapshot protocol protects against interrupted guest writes; it cannot guarantee durability if the host storage stack loses or reorders acknowledged writes after host power loss.

## Separate persistent data disk

`make` creates `build/baseos-data.img` through `tools/init_data.py` only when absent. The initializer never reformats or truncates an existing file: it verifies the exact size and marker, obtains QEMU-compatible locks, and leaves all bytes unchanged. Existing unknown files are rejected. Both images and backups survive `make clean`. Preserve both images when moving a saved installation; the boot floppy alone may contain an older state after migration.

The 16 MiB disk has 32,768 sectors. Sector 0 is an explicit BaseOS data marker: seven little-endian 32-bit words holding magic `0x44534F42` (`BOSD`), marker version 1, sector count 32768, slot size 16383, slot LBAs 1 and 16384, and CRC32 of the first 24 bytes. Remaining marker bytes are zero. Snapshot slots reserve 16,383 sectors each; the last disk sector is unused. A generic blank disk without this marker is never initialized.

Each v4 snapshot uses the existing 28-byte checksummed header in one sector and 40-byte v3-compatible node records followed by exact binary content. Version 4 permits 2,097,152 bytes per file. IDE volumes support 256 total nodes, including root, folders, apps and ordinary files; floppy volumes retain 64. The record layout, unsigned 16-bit node IDs, signed 16-bit parent IDs, v4 version, marker, slot positions and atomic commit protocol are unchanged. The current kernel and host tool read existing v1/v2/v3 floppy and v4 IDE snapshots byte-for-byte; larger IDE trees require the updated kernel and host tool. Do not downgrade an expanded data disk to an older 64-node build.

IDE file-data capacity is `(16383 - 1) * 512 - 40 * max(64, used_nodes)`. It remains exactly **8,385,024 bytes** through 64 nodes, so a full old v4 volume is readable, writable and never truncated by the expansion. Each additional node consumes 40 bytes of that allowance; at 256 nodes it is **8,377,344 bytes**. Admission checks for new files, folders and apps reserve their record before mutation. A full 64-node volume must release at least 40 file bytes before creating node 65. Deleting a node beyond the first 64 returns its 40 bytes, as well as any file contents. Writes and snapshot validation use the allowance for the actual node count, and serialization independently checks the complete payload against its slot. `fs_capacity()` reports the current allowance, `fs_capacity_for_nodes(count)` supports multi-file admission preflights, and `fs_node_limit()` reports 64 or 256 for the mounted backend. File data and metadata therefore cannot overbook a snapshot. Floppy snapshots continue using v3 and their original capacity.

Tree depth is separately capped at **63 non-root components**, preserving every previously valid 64-node tree. Each name is at most 23 bytes, so the longest supported canonical path is 1,512 bytes and fits the 1,536-byte path buffer including its NUL. Create, rename, move and recursive copy check the affected paths before mutation; both kernel and host snapshot imports check the same depth and path-length bounds. Recursive copy/delete remain bounded by 63 levels rather than the wider node count. The path-building stack uses that depth bound. Node-indexed GUI/list/validation buffers still use the 256-node maximum, while capacity displays must use the backend's runtime node limit. Automatic new-file, folder, copy and collision names support three-digit suffixes.

The RAM node table uses 13,312 bytes (256 × 52) of a 64 KiB metadata reservation and holds offsets into an 8 MiB compacting byte arena, rather than reserving 2 MiB for every node. Nonempty files also have a convenience NUL byte outside their reported size. Writes check all limits before changing metadata or bytes. Removal/resizing compacts the arena; equal-length overwrites stay in place. Source aliases from `fs_data()` are staged first, so self-overwrites and file copies survive moves. `fs_data()` returns a borrowed pointer valid only until the next filesystem mutation. Consumers needing long-lived data must copy it. Editor is independently bounded to 65,535 bytes; BASIC/scripts/native images keep their 16,383-byte limits. General files use `fs_file_limit()`.

Mount prefers a valid data volume. When both marked data slots are wholly blank, it first mounts the boot floppy, then stages those files for the first data-disk save. It never writes the floppy during migration, and never migrates from an incompletely readable source. Legacy 1.44 MB source geometry is supported read-only. A missing IDE disk selects ordinary floppy storage. An unknown/corrupt/unreadable IDE volume exposes boot files as a protected recovery view; it does not silently format either disk. A valid surviving snapshot can recover a damaged peer using the existing alternating-snapshot protocol.

The ATA driver supports only legacy primary-master ATA, LBA28, 512-byte logical sectors, polled 16-bit PIO, and an explicitly advertised cache-flush command. It does not mount partitions, SATA/AHCI, USB or arbitrary host disks. Sector bounds and two-second hardware waits are checked. The save sequence writes payload, flushes the ATA cache, reads and verifies payload, writes the commit header, flushes again, then validates the complete snapshot. Uncertain commit outcomes protect the volume until remount. This remains dependent on QEMU and host storage honoring flushes; keep external backups.

The normal command line includes:

```sh
qemu-system-i386 -m 64M -vga std -boot a \
  -drive file=build/baseos.img,format=raw,index=0,if=floppy \
  -drive file=build/baseos-data.img,format=raw,index=0,if=ide,cache=writeback
```

Use `make run QEMU_DATA=` for the optional-disk fallback. Use `python3 tools/volume.py build/baseos-data.img info` for actual capacity and `import`/`export` for byte-preserving exchange while QEMU is stopped. The host updater retains the other snapshot and makes a content-addressed whole-image backup before atomically replacing the target. Kernel rebuilds never rewrite data-disk contents. ATA emulation and cache-option references: [QEMU IDE implementation](https://github.com/qemu/qemu/blob/master/hw/ide/core.c), [QEMU command-line reference](https://www.qemu.org/docs/master/system/qemu-manpage.html).

## Supported machine

BaseOS targets a legacy BIOS Pentium-or-newer x86 PC with PSE, one CPU, drive A with 80 cylinders and two heads, VBE direct-color graphics, PS/2 input, a PIC/PIT, and an ISA DMA floppy controller. Normal runs use QEMU standard VGA, 64 MiB RAM, a 2.88 MB boot floppy, and an optional primary-master ATA data disk. Startup checks the BIOS E820 map for the reserved memory areas and stops if they are unavailable. It accepts matching RGB565 or RGB888 framebuffer formats and stops if video initialization fails. UEFI boot is not supported.

Built-in applications run cooperatively in the kernel. Terminal `start` supports up to eight protected BEX1 tasks, each with a saved 64 KiB image, registers, x87 state and key queue. One task image is mapped into the user region at a time; a PIT slice, yield or sleep returns control to the desktop. There is no cumulative lifetime limit for `start`. Legacy synchronous `exec` retains its two-second watchdog and pauses audio while it runs. Syscalls are checked against the current user region. Kernel exceptions print a serial diagnostic and halt; user-program faults return to Terminal. Only the timer interrupt is enabled among hardware IRQs; PS/2, floppy, ATA, RTL8139 and SB16 device service is polled. There is no general-purpose heap, POSIX process environment or virtual-memory scheduler. See [NATIVE_TASKS.md](NATIVE_TASKS.md) for the bounded task model.

## Checks

```sh
make test
make
python3 tools/smoke_test.py build --keep
python3 tools/extent_test.py build
python3 tools/kernel_space_test.py build
python3 tools/data_volume_test.py build --keep
python3 tools/process_test.py build
python3 tools/ui_test.py build
python3 tools/input_test.py build
python3 tools/render_test.py build --optimized
```

`make test` uses a host C compiler with AddressSanitizer and UndefinedBehaviorSanitizer. It tests image preservation and locking, interrupted commits, corrupt filesystem data, legacy migration, full capacity, copy rollback, floppy/ATA error recovery, RTC snapshots, and memory/video validation. If the host runs under a tracer that prevents LeakSanitizer startup, use `ASAN_OPTIONS=detect_leaks=0 make test`; address/undefined-behavior checks remain enabled.

`ASAN_OPTIONS=detect_leaks=0 python3 -m unittest discover -s tests -p 'test_node_capacity.py'` runs focused ordinary host checks for all four readable format versions, maximum old paths, the full old 64-record/8,385,024-byte volume, 39/40-byte metadata admission, hundreds of files, node ID 255, binary host exchange, recursive copy rollback, move/rename/delete/reclaim and three-digit generated names. It also verifies the floppy's unchanged 64-node limit and original maximum payload. Every image is disposable.

`python3 tools/node_capacity_test.py build --keep` runs five ordinary, serialized QEMU boots using freshly created disks: 256-node creation with 60-level folders and exact binary files, read-only restart, host replacement/export and restart, full old 64-record v4 expansion at the 39/40-byte metadata boundary, then reboot/delete/refill to the original byte capacity. The runner verifies both snapshot decoders, untouched boot-disk bytes, the marker and final reserved sector. It never opens saved `build/*.img` files. These five boots passed on 2026-10-04; the focused sanitizer suite additionally fills the complete 256-node/8,377,344-byte snapshot and copies/deletes a 63-level tree.

A production `-Os -m32 -fstack-usage` check reports 64 bytes per recursive `copy_into` frame, 48 per `fs_delete`, 80 for `tree_fits`, 320 for `fs_path`, 1,248 for complete payload validation, and 1,584 for `fs_destination`. At 63 components, filesystem-owned recursive-copy frames stay below about 4.5 KiB and recursive-delete below 3.2 KiB, before separately bounded device/background callbacks. The node expansion does not multiply recursion depth or path arrays by 256.

`tools/data_volume_test.py` runs thirteen ordinary QEMU boot/restart checks without CPU-fault injection. It verifies exact 2 MiB binary files, host import/export and guest changes, both v4 snapshots at the full 8,385,024-byte data limit, source-alias overwrites, atomic capacity failures, shrink/refill, v3 migration, optional-disk fallback, and byte-preserving protection of unmarked disks. Every disk is disposable; `--keep` retains serial logs and exact disk bytes.

The QEMU suite creates disposable images and checks fresh boot, reboot, timer progress, exception diagnostics, low-memory/video failure, deliberately dirty BSS, and multi-track storage. `--keep` preserves its logs, test disks, and desktop screenshot in the printed temporary directory. No test boots or writes your normal image.

## Memory map

| Range      | Use                         |
|------------|-----------------------------|
| `0x005000` | BIOS E820 memory map        |
| `0x009000`–`0x010000` | temporary protected bootstrap stack |
| `0x010000`–`0x087E00` | temporary BIOS packed-kernel staging (959 sectors) |
| `0x100000`–`0x1F0000` | relocated kernel/code/data/BSS, bounded by linker |
| `0x1F0000`–`0x200000` | reserved 64 KiB kernel stack |
| `0x200000` | 8-bit backbuffer            |
| `0x300000`–`0x310000` | filesystem node table (64 KiB) |
| `0x310000`–`0x500000` | reserved rich-document workspace |
| `0x500000` | Paint canvas / viewer arena |
| `0x700000` | disk DMA bounce buffer      |
| `0x710000`–`0x720000` | SB16 ISA DMA ring          |
| `0xA00000` | cached desktop wallpaper    |
| `0xB00000`–`0xB20000` | Paint undo history |
| `0xB30000`–`0xD40000` | download buffer reservation |
| `0xE00000`–`0xF00000` | independent Terminal state/script buffers |
| `0xDF0000`–`0xE00000` | reserved graphics lookup cache within app arena |
| `0xF00000` | page directory and user page table |
| `0x1000000` | protected native-program region |
| `0x1400000` | cached background during dragging |
| `0x1500000` | last-presented framebuffer mirror |
| `0x1600000`–`0x1610000` | network arena |
| `0x1610000`–`0x1650000` | browser arena |
| `0x1700000`–`0x1900000` | audio source/work arena |
| `0x1900000`–`0x2000000` | reserved image decoding / owned pixels |
| `0x2000000`–`0x2800000` | compact file-data pool (8 MiB) |
| `0x2800000`–`0x3000000` | filesystem snapshot staging (8 MiB) |
| `0x3000000`–`0x3100000` | native task images and a separately bounded 64 KiB syscall stack |
| `0x3100000`–`0x3600000` | Editor documents/undo/clipboard |
| `0x3600000`–`0x3F00000` | video stream/decoder/frame workspace |

Disk LBA 0 contains the loader. The packed kernel uses LBAs 1–383 and the unused tail at LBAs 5184–5759, for **959 sectors / 491,008 packed bytes** total. Snapshot slots remain at LBAs 384 and 2784, each reserving 2400 sectors. No existing filesystem data is moved or reduced. Old 1.44 MB images still upgrade through the byte-preserving image updater and retain their original-image backup.

The canonical debug artifact is `kernel.elf`, linked at **1 MiB**; `kernel.bin` remains its exact `objcopy -O binary` initialized image. `kernel.packed` is a deterministic disk-only derivative produced by the dependency-free Python tool. Compression is data-dependent: the initialized image can exceed the former 491,008-byte raw limit only if its packed artifact fits that reservation, and the complete raw kernel plus BSS must still fit the **960 KiB** RAM range below the stack. This is not unlimited kernel capacity.

The packed artifact has three parts:

1. The first 4096 canonical bytes as a plain bootstrap prefix. The complete real-mode bootstrap, protected-mode decoder, failure diagnostic and initial GDT must fit here, enforced by the linker. A short host-only fixture is zero-padded to 4096 bytes.
2. A 32-byte little-endian envelope, eight `uint32` fields: magic/version `0x314B5042` (`BPK1`), header size 32, codec (0 = raw, 1 = LZ4 block), payload byte length, exact raw output byte length, raw CRC32, reserved zero, and header CRC32 over its first 28 bytes.
3. A payload containing the **entire** canonical initialized image, including its prefix. Codec 1 follows the [LZ4 block format](https://github.com/lz4/lz4/blob/v1.10.0/doc/lz4_Block_format.md); BaseOS has original Python and assembly implementations, with no copied library code or new build dependency. The encoder retains the specified final literal/end-of-block conditions. Codec 0 is selected explicitly for an uncompressed payload inside the same envelope.

The BIOS loader and CHS sector split are unchanged. It joins both packed extents at `0x10000`, checks conventional RAM with INT 12h before reading, and stays below a `0x90000` staging ceiling. BIOS calls and boot-info/E820 buffers remain below the stage. After the last BIOS call, the bootstrap enables A20 with the fast-reset bit cleared and verifies it using two restored probe bytes. A20 failure halts before high-memory writes or C.

The first protected-mode far jump targets the staged plain bootstrap and its staged GDT. With flat segments and interrupts disabled, it sets an explicit low bootstrap stack at `0x9000`–`0x10000`, validates the envelope and expands into the fixed address 1 MiB. It requires the declared output size to equal this bootstrap's linked `__load_end - KERNEL_LOAD_ADDR`; the header cannot choose an execution address. Every token/length/offset read is payload-bounded, every literal or match write is output-bounded, backward offsets must be nonzero and within already produced output, and overlapping matches use a forward byte copy. Exact input/output consumption and both CRC32 checks are required. CRC32 detects accidental corruption; it is not cryptographic authentication. Failure writes `PACKED KERNEL ERROR` to COM1/debug port and halts before C.

After successful reconstruction, the existing handoff reloads GDTR with the high GDT address, jumps to the linked protected entry, establishes the unchanged 64 KiB stack at `0x1F0000`–`0x200000`, zeroes BSS and enters C. E820 validation checks the linked kernel range, high stack and all application arenas. Native ring-3 mappings and their independent syscall/TSS stack at `0x30E0000`–`0x30F0000` are unchanged.

Linker assertions independently bound the plain bootstrap, low stack, staging, complete kernel/BSS, high stack and disk extents. The packed producer enforces the separate on-disk size limit. C assertions and `tools/layout.py` validate the shared ranges from `src/layout.h`. `install_kernel(image, raw)` remains the fixture compatibility API and packs internally; `install_packed_kernel(image, packed)` validates explicit packed input. The image updater's `--packed` flag is used by Makefile; it never guesses a codec from input bytes. Both paths retain image locking, backups, atomic replacement, clearing of only the code extents, and byte-for-byte filesystem preservation. `kernel_offset` now means a packed artifact offset, not an arbitrary raw symbol's offset.

`python3 -m unittest discover -s tests -p 'test_kernel_pack.py'` checks valid literal/overlap/distance fixtures, deterministic real-kernel round trips, explicit raw codec, a 700,000-byte compressible initialized image, and interoperability with system liblz4 when available. `test_kernel_layout.py` checks unchanged arenas, a full 491,008-byte **packed** reservation, both snapshot extents and legacy upgrade preservation without QEMU.

`python3 tools/kernel_space_test.py build` performs six ordinary serialized QEMU boots on disposable disks: production fresh/reboot, the explicit raw codec, a valid upgraded 1.44 MB image, and fresh/reboot with a 640 KiB initialized image plus 256 KiB additional BSS. An unmodified guest is paused with a temporary Unix-socket GDB hardware execution breakpoint just after the decoder's successful checksums, before high GDT/data mutation. QMP compares **all** high initialized bytes against `kernel.bin`, and verifies the low plain prefix allowing only x86-set GDT Accessed bits. The breakpoint is removed and the guest resumes into its normal desktop. Runtime checks then cover EIP, stack, GDTR, task TSS stack, boot-info handoff and zeroed extra BSS. `--quick` runs fresh/reboot only. `python3 tools/extent_test.py build` separately executes ordinary linked instructions placed beyond the old 491,008-byte raw limit, with a packed image spanning both disk extents. No saved build images, guest code/data patches, CPU fault injection or malformed guest inputs are used. Evidence remains in each printed temporary directory.

## Palette

The 256-entry palette is laid out as 16 fixed base colors, a 32-step gray ramp, a 64-entry wallpaper ramp and 16-entry accent ramp that are rewritten whenever the theme changes, and a 5×5×5 color cube for everything else. Gradients dither only between neighboring ramp entries, which keeps them smooth without visible noise.

## Rendering and terminal help

Window drags cache the unchanged desktop behind the moving window. The presenter compares RAM buffers and transfers only changed horizontal spans to video memory. Palette lookups and rounded-corner coverage are cached; bulk fills/copies use x86 word operations. Background app visuals refresh when the drag finishes, while the dragged app is repainted during movement. The idle desktop sleeps until the next timer tick instead of continuously polling. Normal QEMU runs use 64 MiB; all fixed reservations, including file data and staging, require RAM through 63 MiB.

Fonts use four bits per pixel (16 coverage levels), and window masks preserve the background at all four corners. The heavy layered shadows are removed. Font generation uses the included licensed font files and requires Python with Pillow; normal builds use the checked-in generated header.

Terminal opens with a help hint. Use `help` for the command index, `help NAME` or `man NAME` for usage and examples, and Page Up/Page Down to scroll. Output wraps to the window width.

A controlled 20-frame QEMU test with five overlapping windows took 203 timer ticks before the compositor optimizations and 31 afterward: about 7 versus 45 rendered frames per second, including initial cache creation. This measures guest rendering in headless QEMU, not the macOS window's displayed frame rate. The test also checks that cached composition matches a complete redraw pixel-for-pixel. Host tests cover corner clipping, palette invalidation, font access, cursor restoration, and presentation with padded 16-, 24-, and 32-bit framebuffers.

## Runtime display modes

QEMU standard VGA exposes a Bochs DISPI register interface at ports 0x1CE/0x1CF.
The display driver retains the firmware-validated framebuffer mapping and only
programs a bounded table of four 32-bit modes whose indexed backbuffers fit 1 MiB.
Settings confirms each change for 15 seconds and restores the previous mode and
window geometry on cancellation or timeout. A saved preference is loaded after
filesystem mount. Other adapters keep the initial BIOS mode.

Run `python3 tools/display_test.py build` to exercise mode changes, keyboard
confirmation, automatic recovery and persistence. The test prints its screenshot
and log directory. Register reference: https://www.qemu.org/docs/master/specs/standard-vga.html

## Editor working memory

Eight 64 KiB document buffers and eight snapshots per document use a separate
5 MiB arena at 49 MiB. Two 64 KiB scratch/clipboard buffers occupy its final 128 KiB.
Compile-time checks bound all document state below those buffers. Find/replace uses
bounded linear construction and one history step for Replace All; it does not
mutate the original document if the result exceeds capacity.

The native syscall stack lives inside the supervisor-only task arena rather than
kernel BSS. Its 64 KiB budget accommodates device-only MP3 decoding during ordinary
file operations; compiler stack-usage output reports about17 KiB for that decoder
alone. GUI/file callbacks are never dispatched reentrantly from device polling.
