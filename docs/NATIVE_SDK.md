# Native C application SDK

BaseOS runs freestanding i386 applications built by the host SDK. BEX1 remains
the default, with an unchanged 65,536-byte process region. The qualified opt-in
[BEX2 format](BEX2_FORMAT.md) uses a sparse 4 MiB offset extent with text, data,
workspace and stack regions declared and committed at launch; gaps are not
accessible memory. Both use `int 0x80` for bounded platform services. There is no
in-guest compiler, libc, POSIX API, dynamic linker, or heap API.

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
per-file limit remains unchanged. BEX1 application memory remains 64 KiB
regardless of disk capacity. There is no independent BSS length field in the
BEX1 header: the loader clears the whole region before copying the image.

For an opt-in BEX2 build, use `--format bex2`; workspace and stack sizes are
launch declarations, not runtime allocations. See [BEX2 build options and
layout](BEX2_FORMAT.md#building-and-inspecting). BEX2 uses the desktop task route;
synchronous `exec` accepts BEX1 only.

Open a `.bex` file in **Files**, the ordinary **Open** dialog, or search for its
filename with **Ctrl+Space** and press Enter. A program opens in a new Terminal
window with its own protected task, even when another copy is already running.
Native programs have a Terminal icon and appear as applications in Files and as
**Native app** results in the launcher. File names containing spaces and
case-insensitive `.BEX` extensions work without quoting a Terminal command.
Only open programs you trust: protection confines memory and bounds each slice,
but apps can read files and write documents through the documented API.

Or, for a long-lived, interactive or streaming app, run:

```text
start /Programs/docstats.bex /Documents/stats-sample.txt
```

`exec` remains synchronous with its two-second watchdog. `start` gives the app
bounded slices alongside the desktop and other native tasks. Its memory, keys,
registers and x87 state belong to its independent process record; its Terminal
owns the working/published canvases and retains an exact process attachment. See
[NATIVE_TASKS.md](NATIVE_TASKS.md) for lifecycle and scheduling guarantees.

Eight desktop windows and eight task owners remain the limits. A full desktop
reports that a window must be closed; it never takes over another Terminal.
A loader error stays visible in the newly opened Terminal, and the source file
is unchanged. Cached Files/Open/launcher selections reject reused node identities;
launcher names are copied and a renamed search result must be selected again.
The loader validates the current file and copies its bytes before execution.
Closing the Terminal stops the task; minimizing leaves it running. A finished
program leaves its output and the command prompt available.

Counter's **S** and Notebook's document write explicitly call `bos_sync()`.
They report **Saved** only after successful disk synchronization. If the write
succeeds but synchronization fails, the output says **RAM only**; press S again
in Counter or run Notebook again to retry. This flushes the complete filesystem
snapshot, including other pending changes. Existing seeded app files are never
overwritten by an OS rebuild; install an updated example with an intentional
`volume.py ... import ... --replace` while QEMU is stopped.

## DocStats example

DocStats is a real C app that streams complete files in 4 KiB chunks
without keeping the whole file in application memory. Its 320×200 canvas shows
byte, word and line counts and a 256-bin byte-frequency histogram. Low byte values
are at the left; bar heights are normalized to the most frequent byte.

- Pass an input directly: `start /Programs/docstats.bex "/Documents/my notes.txt"`.
- Without a startup argument, default input: `/Documents/stats-sample.txt`.
- Without a startup argument, choose another file by saving its absolute path as the entire contents of
  `/Documents/stats-path.txt`. One trailing LF or CRLF is allowed. The path is at
  most 128 printable ASCII bytes; spaces within names are preserved.
- **R** rereads the startup document, or rereads the path setting when no argument
  was supplied, and streams the input again.
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
cryptographic hash. Reads remain 4 KiB each; DocStats cooperatively yields every
64 KiB and redraws progress every 256 KiB. PIT user-mode preemption remains active
throughout. Its progress arithmetic also covers the opt-in 16 MiB file limit.

## Additive platform services

The shared [platform ABI 1.1](NATIVE_PLATFORM_ABI.md) provides capability/limit
discovery, process identities, versioned file handles, conditional replacement
and owned asynchronous IDE completion at calls 18–27, plus memory discovery at
call 28. Call 29 is the independently negotiated [UI 1.0 gateway](NATIVE_UI.md);
feature bit 6 is conditional on a bound desktop task and available trusted
hosted-input hooks. Existing calls and binaries remain unchanged. Query
capabilities and check results before reading output; neither hosted UI nor
asynchronous sync is available in every execution context/backend.

## Extended legacy ABI

The first thirteen syscall numbers retain their behavior. Default native and
BASIC graphics are still 160×100. Other returning registers are preserved; EAX
holds the result. User pointers are offsets in the process's declared memory:
the contiguous 64 KiB region for BEX1, or accessible launch-declared regions in
BEX2's sparse 4 MiB extent. Syscall transfer limits are the same for both.

| EAX | SDK wrapper | Arguments | Result |
| --- | --- | --- | --- |
| 13 | `bos_read_file_at(path, out, capacity, offset)` | EBX=path, ECX=path length, EDX=output, ESI=capacity≤4096, EDI=unsigned byte offset | Bytes copied, zero at/beyond EOF, or -1 |
| 14 | `bos_canvas_size(width, height)` | EBX=width, ECX=height | 0 for a supported mode, otherwise -1 |
| 15 | `bos_replace_file(path, data, bytes)` | EBX=path, ECX=path length, EDX=data, ESI=bytes≤32768 | Bytes replaced in RAM, -1 on failure, or `BOS_ERR_BUSY` (-2) |
| 16 | `bos_sync()` | None | 0 after a durable filesystem snapshot, or -1 |
| 17 | `bos_argument(out, capacity)` | EBX=output, ECX=capacity | Startup-path byte length, zero if absent, or -1 |

### Startup document argument

`start FILE [DOCUMENT]` accepts one optional existing ordinary file. Both paths
may be double-quoted; relative paths use that Terminal's current folder. The
resolved absolute document path must fit **128 printable ASCII bytes**. A longer
path, empty quoted operand or extra operand is rejected, never silently truncated.
The interactive command line still holds at most 80 bytes, so a short relative
path can be useful in a deeply nested folder. This is one document operand, not
an argv vector, shell expansion, environment block or new entry-point convention.

`bos_argument(out, capacity)` copies that path plus a trailing NUL into the app's
writable memory. The return value excludes the NUL. Capacity zero queries
the length without touching `out`. A nonzero capacity must fit the whole path and
NUL, and the entire declared output range must be writable in that process;
otherwise it returns -1 without a partial copy. An absent argument returns zero
and writes an empty string when capacity is nonzero. The old `exec` and the
no-argument Files/Open/search routes have no startup argument.

The kernel copies and validates startup bytes before accepting a task. A later
source-buffer edit, file rename/deletion, yield, sleep or another task cannot
change its argument. Task and window labels use copied launch basenames. These
labels describe what was launched; they do not follow subsequent renames or
claim that an app has opened the document. The pathname is not a locked file or
snapshot: reads still resolve it again on every call.

### Streaming reads

All read chunks are at most 4096 bytes. The default data volume permits source
files up to 2 MiB. The explicit [large-volume profile](LARGE_VOLUMES.md) permits
16 MiB sources through the same API; an ordinary native reader has streamed a
complete 16 MiB file in that profile. DocStats uses the same storage limits.
BEX1 application memory remains 64 KiB; a larger disk does not change either
format's launch-declared memory. The path must be absolute printable ASCII with
a nonzero length at most 128 bytes. Folders and app shortcuts are rejected. The
complete output range must be writable in the process's mapped regions. An empty
output capacity reads zero bytes. Offsets at or beyond EOF return zero, including
very large unsigned offsets; they never wrap into the beginning of the file.
A final partial chunk copies only the remaining bytes and leaves the rest of
the output buffer unchanged.

`bos_read_file` retains its original start-of-file, at-most-4096-byte behavior.
Those legacy path calls have no file handles or retained filesystem pointers.
New code may use the additive owner-bound [platform ABI](NATIVE_PLATFORM_ABI.md)
for versioned file handles and conditional replacement.

### Canvas modes

Only **160×100** and **320×200** are accepted. Every successful mode change,
including selecting the current mode again, clears the canvas explicitly and
activates it. New `start`, `exec` and BASIC runs, `clear`, and Terminal reset
restore 160×100 with no visible canvas until drawing occurs. A rejected busy
start leaves the live task's canvas alone.

The filled-rectangle syscall (9) accepts widths/heights no greater than the
active canvas, so its largest fill is 320×200 pixels. Pixels outside the canvas
are clipped; empty or wholly offscreen fills do not activate or dirty it. Task
geometry is saved independently across slices; changing one Terminal cannot
resize another.

In native task mode, drawing and resize affect a private working canvas. The
complete frame and its dimensions become visible together at `bos_present`,
`bos_yield`, `bos_sleep` (including zero), or explicit application exit/return.
A positive `bos_sync_wait` (1–60,000 ms) also publishes pending drawing, but only
when the operation is still pending and the call actually suspends. A zero-time
poll or an immediately completed/error result does not publish. `bos_ui_wait`
never publishes. A nonzero application exit code still publishes; timer
preemption, external Stop, exceptions and watchdog termination do not. The
previous complete frame remains visible while drawing is unfinished, including
during a desktop redraw caused by other activity. Resize clears working pixels
immediately but its new
geometry is published only at that same boundary. Calling a boundary without
pending drawing does not copy or redraw the canvas. BASIC and synchronous `exec`
retain their direct drawing/presenter behavior.

The kernel-only `ProgramIO` adapter remains for BASIC and synchronous `exec`.
Its optional `resize(width,height)` and `rect(x,y,width,height,color)` callbacks
follow `present`. Desktop native tasks instead use a copied `ProcessIO` table;
each callback receives the exact `(process, slot, generation)` binding and
validates it before updating the explicit Terminal owner. A null resize callback
returns -1; a null rectangle callback retains the per-pixel `plot` loop.
Terminal's rectangle callback clips once and fills working-canvas rows without
publishing, polling devices or switching tasks. Syscall 9 still checks both
dimensions before accepting even an empty fill and uses each color's low byte.
Kernel integrations must initialize optional fields to zero or a callback.

Selected-Terminal renderer helpers `term_canvas_width()` and
`term_canvas_height()` still exist; native pixels use the published width as a
tightly packed stride. Explicit-owner input and occlusion helpers coexist:
`term_canvas_size(slot, ...)` reads published geometry without changing selection.
`ProcessIO.present` is bounded and must not dispatch other applications;
The shared `app_view`/`app_canvas` implementation supplies native ownership and
copy-only publication through Terminal compatibility wrappers. Terminal retains
its text and command state. This is still hosted Terminal integration, not
independent native-window support.

Historical footprint: the complete-input overflow checkpoint measured 91,516
bytes per Terminal and **732,128 bytes** for eight, leaving 54,304 bytes before
script scratch. Later binding/input fields changed that measurement. The current
structure remains compile-time bounded by the fixed 786,432-byte terminal-state
subarena within the unchanged 1 MiB Terminal arena. Published frames use
512,000 bytes in a separately asserted 512 KiB reservation at `0x600000`–`0x680000`,
in the existing Paint-to-DMA gap. No application-memory or machine-RAM increase
is required; the existing boot E820 validation covers this arena.

### Replacement writes and durability

The old syscall 7 and `bos_write_file` remain capped at 4096 bytes.
`bos_replace_file` accepts at most 32,768 bytes and uses the same document-only
policy: `/Documents` and its existing subfolders. It does not create directories.
The path and complete data range are checked before use.

A replacement either writes the whole file in RAM or returns a negative result
while preserving an existing file. `BOS_ERR_BUSY` (-2), also returned by the old
write syscall, means an incremental disk snapshot temporarily owns filesystem
RAM. No file is created or changed; retry the same path after a short delay.
Native reads, computation and launch remain available during that lease. Other
failures remain -1. If a new file cannot be written, its newly created node is
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

The rectangle backend additionally has finite host equivalence and lifecycle
checks:

```sh
ASAN_OPTIONS=detect_leaks=0 python3 -m unittest discover -s tests -p test_native_rectangle.py -v
ASAN_OPTIONS=detect_leaks=0 python3 -m unittest discover -s tests -p 'test_native_publication*.py' -v
```

These compare the entire working Terminal state against the original scalar
loop at both sizes, including edge clipping, empty/offscreen fills, signed
coordinate limits, byte-color conversion and eight-owner isolation. Production
dispatch checks exercise optional callback and fallback in task and synchronous
contexts, preserving invalid-size results and publication boundaries. The real
Terminal/renderer publication fixture runs with both scalar and bulk fills.
This is host correctness coverage, not evidence of a guest performance gain.
Unchanged-binary guest workload comparisons remain necessary before making any
performance claim. On 2026-10-04, 23 focused host tests passed across rectangle,
publication, rendering, SDK, arguments, launch, titles, platform/file/sync services,
System Monitor and BASIC/Terminal features. A normal freestanding i386 build
also passed without warnings, including the unchanged task/Terminal arena
bounds. QEMU and matched-workload timing were not run for this isolated change.

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


### Native desktop launch verification

`ASAN_OPTIONS=detect_leaks=0 python3 -m unittest discover -s tests -p test_native_launch.py -v`
checks the exact production Files/Open/launcher dispatch functions with the real
filesystem and Terminal. It covers distinct owners, busy-owner preservation,
spaces and uppercase extensions, copied search labels, deleted/reused source
nodes, all eight occupied windows, slot reuse, and the original text/app routes.
A second test executes the actual Counter/Notebook C source against supported
write/sync outcomes and checks that a failed sync never emits Saved.

`python3 tools/native_launch_test.py build` uses a bounded functional-test kernel
and two real QEMU boots with disposable floppy/IDE disks. It checks the actual
protected scheduler, Files and search dispatch, two counters beyond the legacy
two-second limit, independent durable values, Notebook's exact persisted bytes,
eight-window capacity, minimize, Stop, close, source deletion after start and Q
exit. It observes only serial log files and independently parses the stopped
volume. It uses no debugger, monitor, QMP or guest-memory reader. This functional
fixture is distinct from a production-desktop PS/2/pixel test.

### Startup argument regression

`ASAN_OPTIONS=detect_leaks=0 python3 -m unittest discover -s tests -p test_native_arguments.py -v`
checks production Terminal parsing and copied metadata, exact 128-byte canonical
paths, legacy no-argument launches, and production DocStats analysis with complete
2 MiB/16 MiB host fixtures. The real ring-3 companion is
`python3 tools/native_arguments_test.py build`; `--compile-only` prepares its valid
images without starting QEMU. It covers owned argument copies across yield/sleep,
length queries, insufficient-capacity error returns without partial copies,
independent owners, legacy exec/start, quoted/relative Terminal paths, source
rename/deletion and synchronized records across reboot.

### DocStats batching workload

`python3 tools/docstats_workload_test.py build` runs two complete inputs on a
**disposable opt-in large-profile disk** in a 128 MiB QEMU guest. Its comparison
variant is the same current DocStats C source and argument ABI, with the previous
per-4-KiB yield/per-64-KiB redraw policy. The production app yields per 64 KiB and
redraws per 256 KiB; both still perform individual 4-KiB read syscalls.

A 2026-10-04 run measured:

| Input | Previous scheduling | Batched scheduling |
| --- | ---: | ---: |
| 2 MiB | 985 PIT ticks / 14.07 s | 68 ticks / 0.97 s |
| 16 MiB | 7,821 ticks / 111.73 s | 510 ticks / 7.29 s |

A second native Counter continued processing independent input, an idle Terminal
retained its input, and the desktop was redrawn during every scan. The longest
observed scheduler-loop interval was two 70-Hz PIT ticks; this is a measurement,
not a real-time guarantee. Host checks compared every input byte and complete
report (byte/word/line counts and byte sum); all four reports survived reboot.

Original generated PCM played through QEMU's SB16 throughout each measured scan.
The guest required positive played frames and zero driver underruns. The host WAV
check verified sample activity (more than one second of samples and peak amplitude
above 3,000), **not** a full waveform comparison, absence of silent gaps, or physical
hardware latency. Each stream was intentionally stopped after the scan finished.

The first persistence-only reboot attempt exposed a test-harness ordering error:
it required SB16 before taking its completed-run branch, although that reboot has
no audio device. Moving that check after the branch fixed the test. The already
completed data, metrics, PCM and screenshot were retained; the rebuilt guest then
passed the persistence-only reboot. No production scan code changed in that fix.

## Opt-in hosted pointer events

`examples/c/pointer.c` is a regular C client for the pointer service described in
[NATIVE_UI.md](NATIVE_UI.md). Build it as the default BEX1 or with the existing
`--format bex2` option. Kernel startup installs `/Programs/pointer.bex` only
when that name is absent, including on saved disks; an existing file or directory
is preserved. The qualified hosted-UI runtime supports it; it
queries capabilities first and exits normally with an unsupported message on
older kernels or unavailable contexts.

Open one owned endpoint with `bos_ui_host_open(BOS_UI_SUB_POINTER |
BOS_UI_SUB_HOVER | BOS_UI_SUB_WHEEL, &info)`. First consume its mandatory
STATE_RESET. READ returns `BOS_OK` (0) for one fixed 96-byte historical event,
`BOS_PENDING` (1) for an empty queue, or an error; only success changes output.
Events include coordinates and geometry tokens from their original route.
Treat RESET/CANCEL as aborting any local gesture; do not commit because their
accepted buttons became zero. The example keeps each stroke as a preview until
ordinary final UP.

Drain `bos_ui_read` and the unchanged `bos_key` byte queue, explicitly present a
complete frame, then call `bos_ui_wait(target, BOS_UI_WAIT_QUEUE |
BOS_UI_WAIT_LEGACY_KEY, milliseconds)`. WAIT returns `BOS_OK` when an event or
requested legacy key is ready, without consuming it. Wait 0 returns `BOS_PENDING`
if neither is ready; 1–60,000 ms suspends only while pending. UI wait never
publishes a working frame. `BOS_E_TIMEOUT` retains the endpoint; `BOS_E_STALE`
means it must no longer be used. RELEASE returns `BOS_OK` once, then
`BOS_E_STALE` on the same handle; internal owner cleanup is idempotent.
Release/reopen creates a fresh identity and does not turn an already-held
button into a new press.

The endpoint owns no Terminal chrome and creates no window. Captured pointer
coordinates can be negative or outside the logical canvas; clip drawing rather
than rewriting the event coordinates. Host close still forcibly stops the app.
