# Technical reference

[Back to BaseOS](../README.md) · [Desktop and programming guide](../FEATURES.md)

## Build and data safety

Normal builds update only the boot/kernel reservation. Filesystem sectors survive rebuilds and `make clean`. Before updating an existing image, the build saves its previous contents as `build/baseos.img.<hash>.bak`. The updater refuses an image locked by a running QEMU guest; shut the guest down before rebuilding.

The image updater extends an old 1.44 MB image to 2.88 MB without changing its existing volume bytes or LBAs. The kernel reads v1 and v2 filesystems and writes v3, which adds modification timestamps. Its first changed save goes into the second snapshot, leaving the old snapshot intact. Each snapshot holds the full existing capacity: 64 nodes with up to 16,383 data bytes per file.

Saves alternate between two snapshots. A save writes and verifies the new payload, then commits a checksummed header. On boot, the kernel validates both snapshots and loads the newest complete one. It never automatically reformats a corrupt or unreadable disk. The desktop displays a disk warning if changes are only in RAM; Shutdown stays in the desktop if flushing fails. Automatic retries are five seconds apart and stop after three failures. Selecting Shutdown explicitly retries a failed flush, unless a mount or uncertain commit has protected the disk until reboot.

The current split-extent loader requires a 2.88 MB floppy. The image updater upgrades legacy 1.44 MB disks before installing this loader, preserving both the existing volume data and an original-image backup. Keep backups outside the running image for host disk failure or damage to both snapshots. The snapshot protocol protects against interrupted guest writes; it cannot guarantee durability if the host storage stack loses or reorders acknowledged writes after host power loss.

## Supported machine

BaseOS targets a legacy BIOS Pentium-or-newer x86 PC with PSE, one CPU, drive A with 80 cylinders and two heads, VBE direct-color graphics, PS/2 input, a PIC/PIT, and an ISA DMA floppy controller. Normal runs use QEMU standard VGA, 32 MB RAM, and a 2.88 MB floppy. Startup checks the BIOS E820 map for the reserved memory areas and stops if they are unavailable. It accepts matching RGB565 or RGB888 framebuffer formats and stops if video initialization fails. UEFI boot is not supported.

Built-in applications run cooperatively in the kernel. Loadable BEX1 programs run one at a time in a protected 64 KB user region, with checked syscalls and a two-second watchdog. Kernel exceptions print a serial diagnostic and halt; user-program faults return to Terminal. Only the timer interrupt is enabled among hardware IRQs; PS/2 and floppy I/O remain polled. There is no general-purpose heap or background process scheduler.

## Checks

```sh
make test
make
python3 tools/smoke_test.py build --keep
python3 tools/extent_test.py build
python3 tools/process_test.py build
python3 tools/ui_test.py build
python3 tools/input_test.py build
python3 tools/render_test.py build --optimized
```

`make test` uses a host C compiler with AddressSanitizer and UndefinedBehaviorSanitizer. It tests image preservation and locking, interrupted commits, corrupt filesystem data, legacy migration, full capacity, copy rollback, floppy error recovery, RTC snapshots, and memory/video validation.

The QEMU suite creates disposable images and checks fresh boot, reboot, timer progress, exception diagnostics, low-memory/video failure, deliberately dirty BSS, and multi-track storage. `--keep` preserves its logs, test disks, and desktop screenshot in the printed temporary directory. No test boots or writes your normal image.

## Memory map

| Range      | Use                         |
|------------|-----------------------------|
| `0x005000` | BIOS E820 memory map        |
| `0x010000` | kernel, checked below stack |
| `0x080000`–`0x090000` | reserved kernel stack |
| `0x200000` | 8-bit backbuffer            |
| `0x300000` | filesystem node table       |
| `0x500000` | Paint canvas / viewer arena |
| `0x700000` | disk DMA bounce buffer      |
| `0x800000` | on-disk filesystem image    |
| `0xA00000` | cached desktop wallpaper    |
| `0xB00000` | app state and undo storage  |
| `0xF00000` | page directory and user page table |
| `0x1000000` | protected native-program region |
| `0x1400000` | cached background during dragging |
| `0x1500000` | last-presented framebuffer mirror |

Disk LBA 0 contains the loader. The kernel uses LBAs 1–383 and the previously unused tail at LBAs 5184–5695, for 895 sectors total. The loader joins those extents in memory. Snapshot slots remain at LBAs 384 and 2784, each reserving 2400 sectors; their existing on-disk data is never relocated. The linker independently limits the complete kernel and BSS below the stack. All physical addresses, disk boundaries, and the kernel sector budget are defined in `src/layout.h` and checked by the linker, C assertions, and image builder.

## Palette

The 256-entry palette is laid out as 16 fixed base colors, a 32-step gray ramp, a 64-entry wallpaper ramp and 16-entry accent ramp that are rewritten whenever the theme changes, and a 5×5×5 color cube for everything else. Gradients dither only between neighboring ramp entries, which keeps them smooth without visible noise.

## Rendering and terminal help

Window drags cache the unchanged desktop behind the moving window. The presenter compares RAM buffers and transfers only changed horizontal spans to video memory. Palette lookups and rounded-corner coverage are cached; bulk fills/copies use x86 word operations. Background app visuals refresh when the drag finishes, while the dragged app is repainted during movement. The idle desktop sleeps until the next timer tick instead of continuously polling. Normal QEMU runs still use 32 MB; the compositor reservations now require RAM through 22 MB.

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
