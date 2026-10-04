# Desktop features

BaseOS is a small, single-CPU hobby OS. Built-in applications run cooperatively in the kernel; externally loaded native programs run at ring 3 with page protection.

## 1. Window sizing

Drag a window edge or corner to resize it. The square title button and a double-click on the title bar toggle maximize/restore. Drag to the left, right, or top screen edge to snap. Alt+Left/Right snaps to a half of the screen; Alt+Enter toggles maximize.

Editor, Files, and Terminal support small windows. Other applications retain minimum dimensions that fit their controls. The desktop holds eight windows in total.

## 2. Multiple instances

Each Editor, Files, and Terminal window owns its document, folder selection, or terminal state. Opening these apps again creates another instance. File > New (Ctrl+N) creates another window of the active Editor, Files, or Terminal app. Other built-in apps remain single instances.

### Files sorting and filtering

Click Name, Type, Size or Modified to sort; click again to reverse. Ctrl+1 through
Ctrl+4 do the same. Folders stay first and equal keys use stable name ordering.
Ctrl+F opens a case-insensitive, 23-character in-folder name filter. Clear empties
it; Close or Escape clears and exits. Enter/Tab returns to the list while keeping
the filter. The parent row remains available, and the status shows matching/total
counts. Selection follows the same file identity through reordering and clears
when hidden/deleted. See [Files](docs/FILES.md) for controls and safety details.

## 3. Undo and redo

Editor and Paint keep eight undo steps. Use Ctrl+Z to undo and Ctrl+Y or Ctrl+Shift+Z to redo. An editor step is an edit operation; a Paint step is a stroke, shape, fill, text edit, or clear. Editing after undo discards the redo branch. History is held in RAM and resets on restart.

### Editor search and larger documents

Editor holds up to 65,535 bytes in each of its eight independent document buffers,
with eight undo/redo steps. Its clipboard is shared across Editor windows. Files
larger than the document limit are shown in Properties and never silently truncated.
The optional data disk is required to persist documents over 16,383 bytes; a floppy
write that cannot fit fails without replacing the original.

Ctrl+F opens Find; Ctrl+H opens Replace. Enter/F3 find the next match, Shift+Enter/
Shift+F3 find the previous match, and searches wrap. `Aa` toggles case-sensitive
matching. Replace changes the selected match; All replaces non-overlapping matches
as one undoable operation. Ctrl+Enter replaces one and Ctrl+Shift+Enter replaces all.
Tab moves between Find and With fields. Escape dismisses the bar while leaving the
document open. Find fields support insertion, selection, copy/paste and deletion.

Shift+arrows/Home/End extend the selection. Home/End move within the line;
Ctrl+Home/End go to the document boundaries. Page Up/Page Down navigate a screenful,
and Delete removes the selection or next character. Ctrl+S remains available while
the search bar has focus.

## 4. Terminal

Up/Down recalls the last 16 commands for that terminal. Page Up/Page Down scrolls output, which wraps to the window width. `help` lists every command; `man NAME` shows syntax, behavior, limits, and an example. Tab completes a unique command or path; ambiguous matches ask for more characters. Absolute and relative paths support `.` and `..`. Quote paths containing spaces when running commands. Completion handles unquoted paths.

Commands: `help [COMMAND]`, `man COMMAND`, `ls [PATH]`, `cd PATH`, `pwd`, `cat PATH`, `mkdir PATH`, `touch PATH`, `rm PATH`, `echo TEXT`, `clear`, `stat PATH`, `df`, `run SCRIPT`, `basic FILE`, `exec FILE`, `start FILE`, `stop`, and `tasks`.

Scripts contain one terminal command per line; blank lines and lines beginning with `#` are ignored. Execution stops at the first error. Limits are 80 characters per command, four nested scripts, and 256 commands per invocation. There are no shell pipes, redirection, or environment variables.

Try these inside Terminal:

```text
run /Programs/demo.sh
basic /Programs/demo.bas
exec /Programs/hello.bex
```

## 5. Host file exchange

Shut down QEMU before using the host-side exchange tool. It refuses a locked image, preserves the other filesystem snapshot, and creates a backup before changing the disk.

```sh
python3 tools/volume.py build/baseos-data.img info
python3 tools/volume.py build/baseos-data.img ls /
python3 tools/volume.py build/baseos-data.img mkdir /Projects
python3 tools/volume.py build/baseos-data.img import notes.txt /Projects/notes.txt
python3 tools/volume.py build/baseos-data.img export /Projects/notes.txt recovered.txt
```

Add `--replace` to explicitly replace an existing data file or export destination. Parent folders must already exist. File contents can be binary; names are ASCII, at most 23 characters, and cannot contain `/`. On the data disk, files can contain up to 2,097,152 bytes (2 MiB). Editor supports 65,535-byte documents; command scripts and BASIC sources retain their separate 16,383-byte limits. Native BEX1 images support 49,152 bytes on IDE storage. Large media files are not editable as text. Boot a fresh data image once before importing into it.

The same tool still reads and updates old `build/baseos.img` floppy snapshots, which keep their 16,383-byte per-file limit. Once the data disk has mounted successfully, import new files into `baseos-data.img`; changing the old floppy does not replace the newer data volume.

## 6. Session restoration

Every five seconds, after at least one second without input and while no mouse button or dialog is active, and during Shutdown, BaseOS saves window positions, sizes, order, minimized state, folder paths, terminal working folders, editor drafts/caret positions, and the Paint canvas. The next boot restores them.

Editor drafts and the Paint draft live separately under `/prefs`; autosaving a draft does not overwrite its original document. Ctrl+S still saves the document itself. Terminal output/history, undo history, and game progress are not restored. Closing a document window deliberately removes it from the next restored session.

Session files consume normal filesystem slots and space. A status warning appears if they cannot be saved; Shutdown keeps the desktop open on a session or disk-save failure. Abruptly stopping QEMU can lose changes since the last completed snapshot. Session writes use the same two-snapshot persistence protocol as other files.

## 7. Properties and capacity

Select an item in Files and use File > Properties or Ctrl+I. The pane shows type, bytes, modification time in UTC, used volume bytes, available file/folder slots, and synchronization status. Terminal `stat PATH` and `df` provide the same underlying information.

The data volume has 64 total nodes, including root, folders, application shortcuts, and session files. It holds at most 8,385,024 file-data bytes (just under 8 MiB), independently of its 2 MiB per-file limit. Metadata and a second complete snapshot occupy the rest of the 16 MiB disk. Empty files and folders need nodes but no payload allocation. A write that exceeds either capacity fails without truncating or changing the original file; copying rolls back if the destination cannot fit.

Without a data disk, the floppy retains a maximum of 63 × 16,383 data bytes when every non-root node is a full data file; directories, apps, and session files reduce usable capacity. Data disks use filesystem format v4. Floppy v1/v2/v3 snapshots remain readable, and floppy writes remain v3. Modification timestamps survive migration; older files show an unknown time until changed.

## 8. Keyboard navigation

- Alt+Tab or Ctrl+Tab: cycle windows. Ctrl+M: minimize. Ctrl+W: close.
- Ctrl+N: new window/document. Ctrl+O: Open dialog. Ctrl+S: save Editor/Paint.
- Ctrl+A/C/X/V: select all, copy, cut, paste in Editor.
- F10: open the menu bar. Arrows move through menus; Enter activates; Escape dismisses.
- Tab/Shift+Tab: focus controls in Open/Save dialogs. Arrows select files; Enter opens; Escape cancels.
- Desktop arrows/Tab and Enter: select and open icons when no window is in front.
- Files arrows and Enter: select/open items; Backspace goes up. Selection scrolls into view.
- Settings arrows/Tab: change theme; Space toggles the screensaver.
- Paint Tab/Shift+Tab: cycle drawing tools.

## 9. Tiny BASIC

Create a text file in Editor or import one, then run `basic PATH`. Programs have numbered lines (1–65535), uppercase keywords, and integer variables A–Z. Expressions support parentheses, unary minus, `+`, `-`, `*`, and `/`; 32-bit addition/multiplication wrap. Division by zero is an error.

```text
10 LET A=0
20 RECT A,20,4,4,48
30 WAIT 20
40 LET A=A+4
50 IF A < 150 THEN 20
60 PRINT "Done"
70 END
```

Statements: `PRINT` (a quoted string or expression), `LET`, `GOTO`, `IF expression <|>|= expression THEN line`, `INKEY A`, `WAIT milliseconds`, `PLOT x,y,color`, `RECT x,y,width,height,color`, `REM`, and `END`. The canvas is 160×100 pixels using the desktop's 256-color palette. INKEY returns an ASCII character, 27 for Escape, or zero when no key is available.

Limits: 256 lines, 191 characters after a line number, expression nesting of 16, 10,000 executed statements, and ten seconds per run. WAIT accepts 0–1000 ms. Out-of-canvas drawing is clipped. BASIC is an interpreter within the kernel; its safety comes from bounded parsing and execution, rather than the native-program sandbox.

## 10. Loadable native programs

`exec PATH` loads a BEX1 executable into a cleared 64 KB region and enters 32-bit x86 ring 3. User code/data/stack share that region. Paging permits user access only to those 16 pages; kernel memory, page tables, and device mappings are supervisor-only. TSS stack switching and checked system calls handle transitions. Direct port I/O and privileged instructions fault.

A fault returns to the terminal. `exec` remains synchronous with its roughly two-second PIT watchdog. For an interactive or long-running app, use `start PATH`: up to eight terminal-owned tasks share the desktop, with one bounded user-mode slice per desktop turn. Each has an independent 64 KB image, register frame, x87 state, and input queue. `Present`, `yield`, and `sleep` return to the desktop; a timer also preempts CPU-bound user code. Ctrl+C stops the focused task, and closing its terminal discards it. Minimized tasks continue running. `tasks` lists running/sleeping terminal slots.

This is a small desktop-driven task runtime, not a POSIX process system. Kernel syscalls are bounded but are not preempted; disk, rendering, and built-in app work can delay task scheduling. The user pages are writable and executable, with no NX/W^X guarantee. This is an educational boundary, not a claim of production-grade sandbox security. See [the native task guide](docs/NATIVE_TASKS.md) for lifecycle, scheduling and integration details.

The file begins with four little-endian 32-bit words: magic `0x31584542` (`BEX1`), entry offset (at least 16), exact file length, and reserved zero. The complete executable must fit the BEX1 loader's 49,152-byte image limit; files above 16,383 bytes require the IDE data disk. The SDK reserves the last 16 KiB for stack, including when code and BSS grow. Offsets, including the instruction pointer and syscall pointers, are relative to the start of the user region. Initial stack offset is 65,520. Programs must exit through a syscall rather than return.

Use `int 0x80`, with EAX selecting the operation:

| EAX | Operation | Arguments | Return in EAX |
| --- | --- | --- | --- |
| 0 | Exit | EBX = status | Does not return |
| 1 | Write text | EBX = offset, ECX = length (up to 4096) | Length, or -1 for invalid range |
| 2 | Plot pixel | EBX = x, ECX = y, EDX = palette index | 0; off-canvas pixels are ignored |
| 3 | Read timer | None | 70 Hz tick count |
| 4 | Read key | None | ASCII, Escape=27, or 0 |
| 5 | Present canvas | None | 0 |
| 6 | Read file | EBX=path offset, ECX=path length, EDX=buffer, ESI=capacity (≤4096) | Bytes copied or -1 |
| 7 | Write document | EBX=path offset, ECX=path length, EDX=data, ESI=length (≤4096) | Bytes saved or -1 |
| 8 | File size | EBX=path offset, ECX=path length | File length or -1 |
| 9 | Filled rectangle | EBX=x, ECX=y, EDX=width≤canvas width, ESI=height≤canvas height, EDI=color | 0 or -1 |
| 10 | Yield task | None | 0 in task mode, -1 in synchronous exec |
| 11 | Sleep task | EBX=milliseconds, 0..60000 | 0, or -1 for invalid duration/synchronous exec |
| 12 | Task owner | None | Terminal slot 1..8, or 0 in synchronous exec |
| 13 | Read file at offset | EBX=path, ECX=path length, EDX=buffer, ESI=capacity≤4096, EDI=byte offset | Bytes copied, 0 at/beyond EOF, or -1 |
| 14 | Set canvas mode | EBX=width, ECX=height: exactly 160×100 or 320×200 | 0 and cleared canvas, or -1 |
| 15 | Replace document | EBX=path, ECX=path length, EDX=data, ESI=bytes≤32768 | Complete RAM replacement length, or -1 |
| 16 | Synchronize files | None | 0 after durable snapshot flush, or -1 |

The other general registers are preserved across returning syscalls. Native graphics
appear on Present or when execution finishes in synchronous mode; task changes
appear on the next desktop redraw. Present also yields in task mode. Paths must be absolute ASCII, at most
128 bytes. File writes are confined to `/Documents` and its existing subfolders;
reads reject folders and applications. Read chunks and legacy writes are capped at
4096 bytes. Replacement syscall 15 accepts 32,768 bytes atomically; a failed write
preserves the original. Writes change RAM first. Syscall 16 flushes the complete
filesystem snapshot and reports durability separately. Offset reads can stream
complete 2 MiB files, but separate calls do not form an immutable snapshot.

Native programs start on a 160×100 canvas. Syscall 14 opts into 320×200 and clears
it; the next app and BASIC reset to 160×100. Rectangle bounds follow the active
mode. Each terminal keeps independent pixels and dimensions.
No network API is exposed to native programs. Audio pauses during a synchronous
native program and resumes afterward to avoid replaying a stale DMA buffer.
Task mode does not pause audio. x87 state is isolated between native tasks and
the kernel; the supplied C toolchain uses software floating point and no SSE/MMX.

### Building a C application

`sdk/baseos.h` supplies checked-call wrappers. The host-side builder uses the same
freestanding GCC/binutils toolchain as the kernel; it does not require libc. The
small startup and linker script create the BEX1 header, clear BSS via the loader,
and leave at least 16 KB for the application stack.

```sh
python3 tools/build_app.py examples/c/hello.c build/hello-c.bex
python3 tools/build_app.py examples/c/notebook.c build/notebook.bex
python3 tools/build_app.py examples/c/counter.c build/counter.bex
python3 tools/build_app.py examples/c/docstats.c build/docstats.bex
```

The examples are installed without replacing existing files. Run
`exec /Programs/hello-c.bex` for graphics and `exec /Programs/notebook.bex` to read
and write a persistent document. The compiler runs on the host; native apps still
have the 64 KB address space. The two-second watchdog applies to `exec` only.
Open two terminals and run `start /Programs/counter.bex` in each for independent
long-running counters. +/- changes the value, Space pauses, S saves to that
terminal slot's `/Documents/counter-N.txt`, and Q/Escape exits. Sleep and yield
wrappers are `bos_sleep(milliseconds)` and `bos_yield()`; `bos_task_id()` identifies
the owning terminal slot. Task memory is not restored after reboot.

Test the C build/run/persistence path with `python3 tools/sdk_test.py build`, and
the task runtime with `python3 tools/task_test.py build`.

`start /Programs/docstats.bex` streams a configurable document into byte/word/line
counts and a 320×200 byte histogram. Edit `/Documents/stats-path.txt` to select an
absolute input path; R reloads, S saves a synchronized per-slot report, and Q exits.
The included 56,812-byte sample requires IDE storage. The [native SDK guide](docs/NATIVE_SDK.md)
documents the extended ABI, memory limits, exact counting rules and bounded
replacement writes. `python3 tools/native_sdk_test.py build` verifies the new
streaming, large-image, canvas, capacity and reboot paths.

`examples/hello.asm` is assembled during every build and installed as `/Programs/hello.bex` if missing. To assemble another example with the same header and ABI:

```sh
nasm -f bin examples/hello.asm -o build/custom.bex
python3 tools/volume.py build/baseos-data.img import build/custom.bex /Programs/custom.bex
```

Native execution requires a Pentium-or-newer CPU with 4 MB page support. The normal QEMU configuration supplies this. Task state occupies one MiB at 48 MiB; all integrated arenas are validated through 63 MiB. Normal runs use 64 MiB.

## 11. Writer formatted documents

Writer is a single-instance companion to the plain-text Editor. Its proportional
page supports bold, italic, underline, heading/body paragraphs, alignment,
selection, shared clipboard and eight grouped undo operations. Ctrl+N and Open
retain changed work until Save, Discard or Cancel is chosen; all normal close
routes use the same owner-bound guard. Native saves require successful disk sync.

The native `.bwr` format preserves up to 32,768 ASCII text bytes and all supported
styles. Plain import normalizes CRLF in an editable copy without overwriting the
source. Ctrl+Shift+E exports a separate `.rtf` for other word processors. RTF is
export-only; Unicode, DOCX, embedded images, pagination and printing are not
implemented. Complete styled drafts and the caret recover after reboot. See the
[Writer guide](docs/WRITER.md) for shortcuts, exact bounds and verification.

## Verification

```sh
make test
make
python3 tools/smoke_test.py build --keep
python3 tools/process_test.py build
python3 tools/ui_test.py build
python3 tools/input_test.py build
python3 tools/data_volume_test.py build --keep
```

Host tests use AddressSanitizer/UndefinedBehaviorSanitizer for C code and exercise filesystem migrations, undo/redo, BASIC parsing and execution limits, independent terminal state, paths, completion, scripts, and host exchange/locking. QEMU tests cover foundational boot/storage behavior, native faults and privileged operations, syscall bounds, watchdog recovery, independent app instances, BASIC/native execution, Paint history, and a complete session reboot. A further QEMU test types through the emulated PS/2 keyboard, opens Terminal through the launcher, and verifies BASIC INKEY during execution. Disk waits collect input without running app actions. Each QEMU test uses disposable images.

## 12. Spreadsheet

A single-instance 128-row, 26-column Spreadsheet is available from the green grid
desktop icon, launcher and `.bsh`/`.csv` files. Its formula bar preserves source
text while cells show calculated values or explicit errors. Arithmetic, A1
references and SUM/AVG/MIN/MAX/COUNT work with three-decimal fixed-point values.
Range copy/cut/paste, four-operation undo/redo, keyboard/mouse selection, address
jumps and scrolling fit the 800 x 600 desktop and a 420 x 260 minimum client.

Native BSH1 files retain formulas and kinds. CSV imports create unsaved native
copies; CSV exports calculated values separately. Existing unrelated filenames,
changed native sources and reused node IDs are protected. New/Open/Close share
the conservative Save/Discard/Cancel guard. Versioned recovery snapshots include
pending cell edits and pair the draft, saved session and original source before
rebinding. Missing or changed metadata recovers a separate unsaved copy.

An original small budget and plain-text guide are installed only when their
filenames are absent. Limits are explicit: ASCII only, 95 bytes per cell, no
charts, workbook tabs, Excel compatibility or relative-reference rewriting. See
[Spreadsheet guide](docs/SHEET.md) and [format/formulas](docs/SHEET_MODEL.md).
