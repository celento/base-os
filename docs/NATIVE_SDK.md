# Native C application SDK

BaseOS runs its own freestanding i386 BEX1 applications. The SDK compiles on the
host; there is no in-guest compiler, libc, POSIX API, dynamic linker, or heap API.
Apps run with ring-3 isolation in a 65,536-byte region and use `int 0x80` for
bounded text, canvas, keyboard, timer and document operations.

## Build and install

```sh
python3 tools/build_app.py examples/c/docstats.c build/docstats.bex
python3 tools/make_stats_fixture.py build/stats-sample.txt
```

The normal desktop installs missing example files. Existing examples and documents
are preserved. The 56,812-byte stats sample requires the IDE data disk; it is
skipped in floppy-only mode. You can import your own application and input while
QEMU is shut down:

```sh
python3 tools/volume.py build/baseos-data.img import build/docstats.bex /Programs/docstats.bex
python3 tools/volume.py build/baseos-data.img import build/stats-sample.txt /Documents/stats-sample.txt
```

Use `--replace` only when you intend to replace an existing file. Boot a newly
marked data disk once before using the host exchange tool.

A BEX1 file may contain up to **49,152 bytes** including its 16-byte header. The
builder rejects larger code/data images and code/data/BSS that extend beyond the
first 48 KiB. The remaining 16 KiB is reserved for the stack; the initial stack
pointer is offset 65,520. A handwritten executable must observe the same layout.
An executable over 16,383 bytes needs the IDE data volume because the floppy's
per-file limit remains unchanged. Application memory remains 64 KiB regardless
of disk capacity. There is no independent BSS length field in the BEX1 header:
the loader clears the whole region before copying the image.

For a long-lived, interactive or streaming app, run:

```text
start /Programs/docstats.bex
```

`exec` remains synchronous with its two-second watchdog. `start` gives the app
bounded slices alongside the desktop and other native tasks. Its canvas, memory,
keys, registers and x87 state belong to its Terminal slot. See
[NATIVE_TASKS.md](NATIVE_TASKS.md) for lifecycle and scheduling guarantees.

## DocStats example

DocStats is a real C app that reads complete files up to 2 MiB in 4 KiB chunks
without keeping the whole file in application memory. Its 320×200 canvas shows
byte, word and line counts and a 256-bin byte-frequency histogram. Low byte values
are at the left; bar heights are normalized to the most frequent byte.

- Default input: `/Documents/stats-sample.txt`.
- To choose another file, save its absolute path as the entire contents of
  `/Documents/stats-path.txt`. One trailing LF or CRLF is allowed. The path is at
  most 128 printable ASCII bytes; spaces within names are preserved.
- **R** rereads the path setting and streams the input again.
- **S** writes `/Documents/stats-N.txt`, where N is the Terminal slot, then checks
  the disk-sync result. A failed sync is reported as a pending RAM-only save.
- **Q/Escape** exits. **Ctrl+C** stops it from the desktop.
- Missing input or an invalid setting reports an error. Fix the setting, then R
  retries without restarting the app.

Words are nonempty byte runs separated by ASCII space, tab, LF, vertical tab,
form feed or CR. Lines are LF-terminated lines plus a final nonempty unterminated
line. Empty files have zero words and lines. Counts describe bytes, not Unicode
characters. The supplied sample has 56,812 bytes, 9,021 words and 904 lines.

Each streaming call resolves the current path afresh. It is not a filesystem
snapshot. DocStats rejects a length change observed during a read, but it cannot
detect same-length concurrent edits. Avoid editing an input while analyzing it.
The saved report includes a simple byte sum for reproducibility; it is not a
cryptographic hash.

## Extended ABI

The first thirteen syscall numbers retain their behavior. Default native and
BASIC graphics are still 160×100. Other returning registers are preserved; EAX
holds the result. User pointers are offsets in the isolated 64 KiB region.

| EAX | SDK wrapper | Arguments | Result |
| --- | --- | --- | --- |
| 13 | `bos_read_file_at(path, out, capacity, offset)` | EBX=path, ECX=path length, EDX=output, ESI=capacity≤4096, EDI=unsigned byte offset | Bytes copied, zero at/beyond EOF, or -1 |
| 14 | `bos_canvas_size(width, height)` | EBX=width, ECX=height | 0 for a supported mode, otherwise -1 |
| 15 | `bos_replace_file(path, data, bytes)` | EBX=path, ECX=path length, EDX=data, ESI=bytes≤32768 | Bytes replaced in RAM, or -1 |
| 16 | `bos_sync()` | None | 0 after a durable filesystem snapshot, or -1 |

### Streaming reads

All read chunks are at most 4096 bytes, even when the source file is 2 MiB. The
path must be absolute printable ASCII with a nonzero length at most 128 bytes.
Folders and app shortcuts are rejected. The complete output range must be inside
the process region. An empty output capacity reads zero bytes. Offsets at or
beyond EOF return zero, including very large unsigned offsets; they never wrap
into the beginning of the file. A final partial chunk copies only the remaining
bytes and leaves the rest of the output buffer unchanged.

`bos_read_file` retains its original start-of-file, at-most-4096-byte behavior.
There are no file handles or retained filesystem pointers.

### Canvas modes

Only **160×100** and **320×200** are accepted. Every successful mode change,
including selecting the current mode again, clears the canvas explicitly and
activates it. New `start`, `exec` and BASIC runs, `clear`, and Terminal reset
restore 160×100 with no visible canvas until drawing occurs. A rejected busy
start leaves the live task's canvas alone.

The filled-rectangle syscall (9) accepts widths/heights no greater than the
active canvas, so its largest loop is 320×200 pixels. Pixels outside the canvas
are clipped. Task geometry is saved independently across slices; changing one
Terminal cannot resize another. `present` still yields in task mode.

The kernel `ProgramIO` structure appends an optional `resize(width,height)`
callback. A null callback causes the new resize call to return -1. Existing
compiled BEX1 apps are unaffected. Kernel integrations must initialize the new
field to zero or a callback. The renderer reads `term_canvas_width()` and
`term_canvas_height()`; pixels use the active width as a tightly packed stride.

A Terminal now occupies 91,468 bytes, including 320 scrollback rows and all
64,000 possible canvas pixels. Eight use **731,744 bytes**, below the fixed
786,432-byte terminal-state subarena, with 54,688 bytes spare before script
scratch. The overall 1 MiB Terminal arena and other memory mappings are unchanged.

### Replacement writes and durability

The old syscall 7 and `bos_write_file` remain capped at 4096 bytes.
`bos_replace_file` accepts at most 32,768 bytes and uses the same document-only
policy: `/Documents` and its existing subfolders. It does not create directories.
The path and complete data range are checked before use.

A replacement either writes the whole file in RAM or returns -1 while preserving
an existing file. If a new file cannot be written, its newly created node is
removed. Empty replacement writes are supported. The current filesystem's
per-file and total-capacity limits still apply; over-16,383-byte writes need IDE.
The 32 KiB buffer must coexist with the app's code, data, BSS and stack, so it is
not an additional memory allocation or a larger process address space.

A successful write is not by itself durable. `bos_sync` commits and flushes the
whole filesystem snapshot, including other applications' pending writes. It
returns zero only on successful synchronization. Failure can leave the new data
in RAM and pending for a later retry. It is not a per-file transaction and does
not roll a failed synchronization back to the old RAM contents. Disk work is
bounded but runs in the syscall's kernel context and can delay other tasks.

## Verification

```sh
ASAN_OPTIONS=detect_leaks=0 python3 -m unittest discover -s tests -p test_native_sdk.py -v
ASAN_OPTIONS=detect_leaks=0 python3 -m unittest discover -s tests -p test_sysmon.py -v
make
python3 tools/native_sdk_test.py build
python3 tools/task_test.py build
python3 tools/sdk_test.py build
```

The focused host suite checks legacy C example builds, the 34,144-byte C fixture,
the deterministic text fixture, all eight full-resolution canvases, exact arena
footprint, clipping, same-mode clearing and legacy lifecycle resets under
ASan/UBSan. The expanded QEMU test boots disposable disks only and verifies:

- An exact 49,152-byte executable runs normally.
- Two 34,144-byte C programs each stream and verify an entire 2 MiB file in 512
  chunks, retaining separate images, output and canvases.
- Legacy reads, zero-capacity reads, exact/beyond EOF and partial tails agree.
- 32 KiB document replacements and explicit sync survive a real guest reboot.
- DocStats computes and durably saves the supplied sample and an empty file.
- Legacy graphics/notebook apps and BASIC retain their original default mode.
- Full-volume replacement failures preserve the old document and node count,
  including after reboot.

The existing task regression additionally verifies x87 isolation, sleep, input,
preemption, stop/restart and the synchronous watchdog. The old SDK regression
checks its original graphics and notebook persistence path. These focused checks
use normal valid images and supported operations, with no intentional CPU/memory
fault probes or malformed-input fuzzing. Physical hardware and simultaneous
same-length file edits are outside this verification.


## Desktop integration verification

`tools/docstats_input_test.py` starts the seeded C application through the normal
Terminal, checks the 56,812-byte original sample against the host generator, and
verifies 9,021 words and 904 lines in two independently synchronized reports.
The software framebuffer matches the owned 320x200 canvas exactly, including its
640x400 nearest-neighbor enlargement when maximized. Starting the old counter app
restores the legacy 160x100 canvas. Both reports and the unchanged sample survive
an actual reboot. All images are disposable.

The sample is generated compactly into existing startup scratch memory, rather
than spending 56 KiB of boot-image space on repeated text. A host equality test
verifies every generated byte. Seeding skips an existing destination and skips
the oversized sample in floppy-only mode. User files are never replaced by seeding.
