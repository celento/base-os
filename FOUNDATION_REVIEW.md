# BaseOS foundation review

The original assessment is retained below. The fixes are implemented; see [repair status](#repair-status-september-9-2026) for the changes and verification.

Reviewed September 9, 2026. Scope: boot, C startup, memory ownership, exceptions, display initialization, timing, filesystem persistence, and build behavior. No feature or implementation changes were made.

## Verdict

Keep this codebase. The current architecture is reasonable for a tiny, single-address-space hobby OS with built-in applications. It boots a real freestanding kernel and has useful separations for rendering, filesystem operations, storage, and several apps. A monolithic kernel, fixed-capacity storage, polling, and the absence of networking or POSIX are not reasons to rewrite it.

However, fix the correctness and recovery issues below before substantially expanding it. Working in the default QEMU configuration is narrower than having a reliable startup and storage contract.

## Verification

- Built all sources using `make OUT=/private/tmp/baseos-foundation-review -j4`, preserving the existing build image and its saved files.
- No C compiler warnings. The linker warns about a read/write/execute LOAD segment. With the current flat address space and no paging, changing ELF permissions alone would not provide runtime protection.
- Kernel binary: 144,212 bytes against a 163,840-byte loader budget.
- Inspected ELF sections. `.bss` occupies `[0x33360, 0x37001)`. The loader reads through address `0x38000`, so the current image's zero padding happens to initialize all of it.
- Booted the separate image with 32 MB RAM, standard VGA, snapshot disk writes, and no display for 20 seconds. Serial output reached `Kernel started`, `FS ready`, `Mouse ready`, `SPLASH`, and `DESKTOP`. QEMU selected 1280×720×32 with framebuffer `0xFD000000`.
- This is a source review and boot smoke test, not real-hardware certification, an exhaustive app audit, or a storage fault-injection test. Failure-path findings below come from the code unless stated otherwise.

## Fix before adding substantial features

### 1. Rebuilding destroys saved files

`Makefile:62` recreates the entire floppy image whenever the kernel or boot sector changes. The filesystem lives in that same image starting at LBA 384. Consequently, a normal code edit followed by rebuilding replaces the persisted volume with zeros.

Make image initialization an explicit operation. Normal builds should update only the reserved boot/kernel sectors in an existing image, or use a separately managed data image with corresponding driver support. Add a regression check that places data in the volume, rebuilds, and verifies it survives. Preserve the user's existing image before implementing this.

### 2. Disk failures can turn into permanent data loss

`src/fs.c:554` writes the new header and payload over the sole previous snapshot. A partial write can invalidate both the old and new state. The checksum detects damage but cannot recover the previous version.

`src/kernel.c:6115` treats every load failure as a fresh filesystem. `fs_init` has already marked the seeded files dirty, and the main loop later autosaves them. A transient read failure or damaged image can therefore lead to overwriting recoverable data with defaults. `fs_sync` also discards the save result, leaving failures invisible and eligible for immediate repeated attempts.

Distinguish an unformatted disk from a read error or corrupt volume. Preserve the disk on load failure, expose save failures, and introduce a bounded retry policy. Design a recoverable commit scheme before promising durable saves. Two maximum-size snapshots will not fit on this floppy with the current capacity, so redundancy requires an explicit capacity or storage-layout decision.

### 3. Startup does not establish all C runtime requirements

`src/kernel_entry.asm:313` sets segments and the stack, then calls C without explicitly clearing `.bss`. The current build works because the loader reads zero-filled padding beyond the binary. The binary-size check excludes `.bss`, so a future larger static array can break this silently.

Neither assembly entry path establishes a clear direction flag before string instructions. The initial `rep stosw` operations themselves depend on it, as does the C ABI's direction-flag expectation.

Add `cld` at entry and before entering C. Export `.bss` bounds from the linker, clear that range explicitly, include COMMON and subsection handling, and assert that the complete kernel memory footprint leaves a reserved stack region. Check startup with deliberately nonzero `.bss` memory rather than relying exclusively on QEMU's defaults.

### 4. CPU exceptions have no usable handling path

There is no protected-mode IDT installation. Keeping maskable interrupts disabled does not disable CPU exceptions or NMIs. A divide error or invalid instruction can escalate into a double/triple fault instead of producing a diagnostic.

Install exception gates early, with correct handling of exception error codes, and a minimal serial panic path that reports the vector and instruction address before halting. A scheduler and device IRQ conversion are not prerequisites for this. Test intentional divide-error and invalid-opcode faults in a disposable test build.

### 5. Memory ownership exists only as scattered constants

The current selected display modes fit between the 2 MB backbuffer and 3 MB filesystem table. The other fixed arenas also appear separated for the present sizes. But no firmware memory map is collected, and no common reservation mechanism checks RAM availability, reserved regions, the stack, or arena growth.

`src/kernel.c:1252` accepts dimensions up to 2048×2048. Such a one-byte-per-pixel backbuffer would overwrite the filesystem table. The current loader chooses smaller modes, so this is a broken validation boundary rather than a demonstrated default-boot collision.

Centralize addresses and capacities, add compile/link assertions for fixed areas, and validate the actual framebuffer size. Collect and validate firmware RAM information before claiming support beyond the configured QEMU machine. A general-purpose heap or paging system can wait until a feature actually needs it.

### 6. Graphics failure handling can write to an invented address

`src/kernel_entry.asm:141` uses `test ax, 0x0091` followed by `jz` to check mode attributes. This accepts any one of the requested bits; it does not require all three. Require supported, graphics, and linear framebuffer attributes together.

`src/kernel.c:1271` proceeds with an unverified framebuffer address after all mode-setting attempts fail. Stop with a serial error instead. The pitch check must require at least width times bytes per pixel, and accepted formats must match the renderer's channel layout. The renderer currently assumes fixed RGB layouts without validating VBE color masks.

The successful QEMU boot used the firmware-provided address, so the unsafe fallback was not exercised by the smoke test.

### 7. Display polling is used as elapsed time

`src/kernel.c:1216` counts observed VGA retrace transitions. Rendering and synchronous disk I/O can miss transitions. Code then divides that count by 70 for uptime and uses it for gameplay, double-click timing, animation, and the splash exit condition. These timings vary with workload and display behavior; the splash has no independent timeout if retrace never advances.

Introduce a monotonic hardware clock and base deadlines on elapsed time. This can remain a single cooperative event loop. Device interrupt handling and sleeping while idle can follow separately.

### 8. Floppy recovery does not restore the requested track

`src/persist.c:255` recalibrates after a failed transfer, then retries without seeking back to the requested cylinder. Recalibration moves the head to cylinder zero. Retrying a transfer for a later cylinder therefore does not restore its physical preconditions.

Seek to the requested cylinder on every retry. Propagate all command-byte failures, reset/reinitialize the controller when a transaction loses synchronization, and ensure DMA is masked on every exit. Validate result status and transfer completion before reporting success. The current driver also assumes drive A and 1.44 MB geometry; either make that an explicit supported-machine constraint or pass the boot drive through startup.

## Smaller follow-ups

- `src/fs.c:474`: recursive copying ignores failures in child copies, so a full node table can produce a partial copy reported as success. Roll back or explicitly report partial completion.
- `src/rtc.c:28`: a single RTC read after checking update-in-progress can straddle a clock update. Read matching stable snapshots with a bounded failure path.
- `Makefile:26` and `src/fs.c:19`: the loader sector budget and filesystem start are independent. Assert the kernel reservation ends before LBA 384. Increasing the loader budget as the current error suggests must not consume filesystem sectors.
- Keep the 6,332-line desktop module working while gradually extracting hardware startup/input/timing behind small interfaces. Its size makes changes harder to review, but splitting files alone will not fix the bugs above.

## Recommended order

1. Protect the existing disk image from rebuilds and failed mounts.
2. Establish deterministic startup, exception reporting, and checked memory reservations.
3. Make graphics initialization fail safely and replace retrace-based elapsed time.
4. Repair storage retries and design recoverable persistence within an explicit disk-capacity budget.
5. Add focused regression tests for those contracts, then resume feature work.

The desktop, rendering code, and apps can survive all of these changes. There is no evidence here that a whole-project rewrite is necessary.

## Reference checks

- Intel Software Developer Manuals, system programming volumes, for exception delivery, IDT setup, and processor state: https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html
- GNU linker documentation, runtime initialization example including explicit BSS clearing: https://ftp.gnu.org/old-gnu/Manuals/ld-2.9.1/html_node/ld_21.html

## Repair status, September 9, 2026

The findings above describe the original implementation. The foundation repair now addresses them without replacing the desktop or apps:

- Image updates preserve the volume, refuse locked images, and back up prior image contents. Clean preserves images and backups.
- The image grows to a 2.88 MB floppy to hold two full-capacity snapshots. Existing bytes stay at their original LBAs. Legacy v1 snapshots load and migrate on the next changed save. New snapshots use CRC32 for payload and commit metadata, payload verification before commit, and alternating slots. Mount errors protect the disk. Autosave failures remain dirty, show a desktop warning, and retry at most three times with five-second spacing. A failed flush does not power the machine off.
- Startup explicitly clears BSS and DF, checks A20, and enforces entry, image, BSS, and stack bounds. The linker includes subsection and COMMON handling and separates executable and writable ELF segments.
- The IDT reports exception vector, error code, and EIP over serial. PIC IRQ0 drives a PIT clock; all application work remains in the existing cooperative loop.
- The BIOS memory map and shared reservations are checked before arena use. Video initialization requires all necessary VBE attributes, sufficient pitch and buffer capacity, a nonzero framebuffer address, and supported channel layouts. Unverified fallback addresses were removed.
- Floppy commands propagate failures, reset and seek again on retry, validate status and DMA terminal count, use timer-based deadlines, and mask DMA on failure paths. Drive A and the supported geometries are explicit.
- Recursive copy failures roll back the partial destination. RTC reads require matching snapshots and retain the last valid reading after bounded failure.

Validation uses sanitizer-enabled host tests and disposable QEMU images. The QEMU checks cover fresh boot, reboot persistence, a 70 Hz hardware clock, divide/invalid-opcode/protection-fault diagnostics, low RAM, missing graphics, dirty BSS, and multi-track reads/writes through both snapshot slots. See README for reproducible commands and remaining platform limits.

Source-level layout and API contracts are checked, but this remains a single-address-space hobby OS. The work does not claim real-hardware certification or protection against host storage failure. CPU exception diagnostics do not substitute for memory isolation.

Final verification: all seven host test cases passed with sanitizers; all nine disposable QEMU boot scenarios passed. The timer measured 140 ticks in two seconds. Image locking was verified against a running QEMU guest, and a clean/rebuild preserved the test volume. The original user image contains 27 v1 nodes; its volume bytes remained unchanged during upgrade, its backup was verified byte-for-byte, and an upgraded copy loaded successfully to the desktop.


## Desktop expansion, September 18, 2026

The single-address-space description above records the foundation audit at that time. Loadable native programs now use ring 3, supervisor/user page permissions, checked syscalls, and a timer watchdog. Built-in apps remain cooperative kernel code. Filesystem v3 adds modification times while retaining v1/v2 readers. See FEATURES.md for the implemented features, limits, and additional tests.
