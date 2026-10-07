# BaseOS: runtime and developer quick start

BaseOS is a custom BIOS/i386 operating system written in C and x86 assembly, with its own graphical desktop, productivity tools and protected native tasks. It moves toward a small 1990s-style desktop; it does not claim Windows 95/98 application compatibility or feature parity.

## Build and run

Build dependencies are Python 3, NASM, QEMU and an ELF-targeting GCC/binutils toolchain. The top-level [README](../README.md) lists host package commands for Linux and macOS. From the repository root:

```sh
make            # build the boot floppy and create the data disk if missing
make run        # boot the default profile in a QEMU window
make headless   # same, with a serial console and no window
make run-large  # boot the large profile
make headless-large
```

| Profile | Guest RAM | Boot/data image pair | File limits |
| --- | --- | --- | --- |
| Default (`make run`) | 64 MiB | `build/baseos.img` + `build/baseos-data.img` | 16 MiB data disk; 2 MiB/file; about 8 MiB total content |
| Large (`make run-large`) | 128 MiB | `build/baseos.img` + `build/baseos-large-data.img` | 64 MiB data disk; 16 MiB/file; about 32 MiB total content |

Both IDE profiles support 256 total file, folder and application nodes. Metadata slightly reduces content capacity. The two data disks are independent and do not share later changes. Back up a data disk you care about and run one emulator per disk. Normal rebuilds preserve existing filesystem data; `make clean` removes build objects but keeps disks and backups. `make test` runs the host test suite.

Use **System → Shutdown** and allow saves to finish before closing QEMU. Stop QEMU before rebuilding or exchanging files with an image. Audible output depends on the host audio backend.

## Start exploring

Press **Ctrl+Space** to search apps and files; use **Alt+Tab** to switch windows. Up to eight windows can be moved, resized, minimized, maximized or snapped. Settings offers four display sizes through 1280 × 800, themes and timed display-mode reversion.

- **Editor:** plain-text documents up to 65,535 bytes, selection, clipboard, undo and find/replace.
- **Writer:** up to 32,768 ASCII text bytes, proportional wrapping, headings, bold/italic/underline, alignment, find/replace and grouped undo. Save native `.bwr` documents; export separate RTF or Letter/A4 PDF files. A sample guide is at `assets/examples/writer-guide.bwr`.
- **Spreadsheet:** a 26-column × 128-row sheet, bounded formulas, three-decimal fixed-point arithmetic, number/currency/percent display, column widths, range clipboard and undo. Save `.bsh`; import/export CSV values. Try `/Documents/budget.bsh`.
- **Files and utilities:** sorting, folder filtering, collision-safe copy/move, Calculator, Todo, Clock, local Calendar appointments, games and System Monitor.
- **Paint and media:** drawing tools; JPEG/PNG/BMP/GIF-first-frame image viewing; PCM WAV/MP3 playback; MPEG-1 video with supported MP2 audio. Original `harbor.mp3` and `harbor.mpg` examples are in `assets/examples`; import them into `/Media` to try them (see below).

Use **Ctrl+S** to save supported documents. Writer exports PDF with **Ctrl+Shift+P** and RTF with **Ctrl+Shift+E**. Built-in document guards and recovery protect supported unfinished work; recovery still requires a completed disk save or normal shutdown.

Writer/Spreadsheet IDE saves and exports return while disk confirmation proceeds, so editing can continue. “Accepted in RAM” and “saved to disk” are different states. Busy or failed saves retain work for retry. Some other explicit saves, encoding and large copies remain synchronous; full-volume synchronization can take substantial time.

## Native programs

Open `/Programs/counter.bex`, `notebook.bex`, `docstats.bex` or `pointer.bex`. DocStats analyzes a document; Pointer demonstrates drawing/input. `/Programs/pointer-window.bex` opens an independent native window. The optional `examples/c/workspace_array.c` and `workspace_index.c` BEX2 examples demonstrate larger launch-declared memory; build and import them as described in [BEX2_FORMAT.md](BEX2_FORMAT.md).

In Terminal:

```text
help
run /Programs/demo.sh
basic /Programs/demo.bas
start /Programs/docstats.bex /Documents/stats-sample.txt
```

The host C SDK supports BEX1 executables up to 48 KiB in a 64 KiB process region. Opt-in BEX2 permits 256 KiB executables with private text/data/workspace/stack regions in a sparse 4 MiB extent, subject to free pages and process policy. Unmapped gaps are inaccessible; this is not a dynamic heap. Eight task owners share the desktop's window limits.

General ABI **1.3** and separately negotiated UI **1.1** provide versioned files, owned asynchronous disk completion, pointer events and owned primary windows. Staged conditional writes support up to 256 KiB per process on IDE; RAM publication requires separate durability confirmation. Existing 32 KiB replacement calls retain their limit.

Native Close/Stop is forced. There is no native save-aware close handshake, structured keyboard-event stream, IPC or native networking API. Native/result windows are not restored with the desktop session. Run trusted programs: memory protection does not remove their documented file access.

## Exchange files

`make run-large` creates a separate large data disk, not a copy of an existing IDE installation. To migrate saved documents, with QEMU stopped and the destination absent:

```sh
python3 tools/migrate_volume.py build/baseos-data.img build/baseos-large-data.img --profile large
```

Boot a newly created data disk once before using the host exchange tool. For file exchange, substitute the exact stopped data-image path:

```sh
python3 tools/volume.py build/baseos-data.img export /Documents/report.pdf report.pdf
python3 tools/volume.py build/baseos-data.img import notes.txt /Documents/notes.txt
python3 tools/volume.py build/baseos-data.img import assets/examples/harbor.mp3 /Media/harbor.mp3
python3 tools/build_app.py examples/c/docstats.c build/docstats.bex
```

Imports require `--replace` to overwrite existing files. Installed examples also survive OS rebuilds.

## Remaining boundaries

This is neither Linux nor POSIX: no in-guest C compiler, libc, dynamic linker, general-purpose virtual-memory API or UEFI support. Built-ins cooperate inside the kernel. Writer has no DOCX, general RTF import, Unicode, guest PDF viewer or printer support. Large storage leaves document models and 2 MiB download/MPEG limits unchanged; it expands WAV/MP3 source capacity to 16 MiB.

Browser is a small HTTP/local-HTML reader with one network request at a time. It lacks HTTPS/TLS, JavaScript, CSS layout, forms and authentication. Network tests use controlled host-network fixtures; unrestricted public internet access is not claimed.
