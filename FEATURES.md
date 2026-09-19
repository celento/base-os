# Desktop features

BaseOS is a small, single-CPU hobby OS. Built-in applications run cooperatively in the kernel; externally loaded native programs run at ring 3 with page protection.

## 1. Window sizing

Drag a window edge or corner to resize it. The square title button and a double-click on the title bar toggle maximize/restore. Drag to the left, right, or top screen edge to snap. Alt+Left/Right snaps to a half of the screen; Alt+Enter toggles maximize.

Editor, Files, and Terminal support small windows. Other applications retain minimum dimensions that fit their controls. The desktop holds eight windows in total.

## 2. Multiple instances

Each Editor, Files, and Terminal window owns its document, folder selection, or terminal state. Opening these apps again creates another instance. File > New (Ctrl+N) creates another window of the active Editor, Files, or Terminal app. Other built-in apps remain single instances.

## 3. Undo and redo

Editor and Paint keep eight undo steps. Use Ctrl+Z to undo and Ctrl+Y or Ctrl+Shift+Z to redo. An editor step is an edit operation; a Paint step is a stroke, shape, fill, text edit, or clear. Editing after undo discards the redo branch. History is held in RAM and resets on restart.

## 4. Terminal

Up/Down recalls the last 16 commands for that terminal. Page Up/Page Down scrolls output, which wraps to the window width. `help` lists every command; `man NAME` shows syntax, behavior, limits, and an example. Tab completes a unique command or path; ambiguous matches ask for more characters. Absolute and relative paths support `.` and `..`. Quote paths containing spaces when running commands. Completion handles unquoted paths.

Commands: `help [COMMAND]`, `man COMMAND`, `ls [PATH]`, `cd PATH`, `pwd`, `cat PATH`, `mkdir PATH`, `touch PATH`, `rm PATH`, `echo TEXT`, `clear`, `stat PATH`, `df`, `run SCRIPT`, `basic FILE`, and `exec FILE`.

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
python3 tools/volume.py build/baseos.img ls /
python3 tools/volume.py build/baseos.img mkdir /Projects
python3 tools/volume.py build/baseos.img import notes.txt /Projects/notes.txt
python3 tools/volume.py build/baseos.img export /Projects/notes.txt recovered.txt
```

Add `--replace` to explicitly replace an existing data file or export destination. Parent folders must already exist. File contents can be binary; names are ASCII, at most 23 characters, and cannot contain `/`. Files are limited to 16,383 bytes. Boot a fresh image once before importing into it.

## 6. Session restoration

Every five seconds, after at least one second without input and while no mouse button or dialog is active, and during Shutdown, BaseOS saves window positions, sizes, order, minimized state, folder paths, terminal working folders, editor drafts/caret positions, and the Paint canvas. The next boot restores them.

Editor drafts and the Paint draft live separately under `/prefs`; autosaving a draft does not overwrite its original document. Ctrl+S still saves the document itself. Terminal output/history, undo history, and game progress are not restored. Closing a document window deliberately removes it from the next restored session.

Session files consume normal filesystem slots and space. A status warning appears if they cannot be saved; Shutdown keeps the desktop open on a session or disk-save failure. Abruptly stopping QEMU can lose changes since the last completed snapshot. Session writes use the same two-snapshot persistence protocol as other files.

## 7. Properties and capacity

Select an item in Files and use File > Properties or Ctrl+I. The pane shows type, bytes, modification time in UTC, used volume bytes, available file/folder slots, and synchronization status. Terminal `stat PATH` and `df` provide the same underlying information.

The volume has 64 total nodes, including root and folders. Its maximum payload is 63 × 16,383 bytes when every non-root node is a full data file; directories, apps, and session files reduce usable capacity. Modified times are stored by filesystem format v3. Existing v1/v2 images remain readable, and older files show an unknown modification time until changed.

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

A fault returns to the terminal. A PIT watchdog terminates execution after roughly two seconds. Only one native program runs at a time, synchronously; there is no background scheduler or general-purpose process API. The user pages are writable and executable, with no NX/W^X guarantee. This is a small educational boundary, not a claim of production-grade sandbox security.

The file begins with four little-endian 32-bit words: magic `0x31584542` (`BEX1`), entry offset (at least 16), exact file length, and reserved zero. The complete file must fit the filesystem's 16,383-byte limit. Offsets, including the instruction pointer and syscall pointers, are relative to the start of the user region. Initial stack offset is 65,520. Programs must exit through a syscall rather than return.

Use `int 0x80`, with EAX selecting the operation:

| EAX | Operation | Arguments | Return in EAX |
| --- | --- | --- | --- |
| 0 | Exit | EBX = status | Does not return |
| 1 | Write text | EBX = offset, ECX = length (up to 4096) | Length, or -1 for invalid range |
| 2 | Plot pixel | EBX = x, ECX = y, EDX = palette index | 0; off-canvas pixels are ignored |
| 3 | Read timer | None | 70 Hz tick count |
| 4 | Read key | None | ASCII, Escape=27, or 0 |

The other general registers are preserved across returning syscalls. Native graphics appear when execution finishes. File/network access is not exposed.

`examples/hello.asm` is assembled during every build and installed as `/Programs/hello.bex` if missing. To assemble another example with the same header and ABI:

```sh
nasm -f bin examples/hello.asm -o build/custom.bex
python3 tools/volume.py build/baseos.img import build/custom.bex /Programs/custom.bex
```

Native execution requires a Pentium-or-newer CPU with 4 MB page support. The normal QEMU configuration supplies this. BaseOS validates its reserved RAM through 22 MB; `make run` uses 32 MB.

## Verification

```sh
make test
make
python3 tools/smoke_test.py build --keep
python3 tools/process_test.py build
python3 tools/ui_test.py build
python3 tools/input_test.py build
```

Host tests use AddressSanitizer/UndefinedBehaviorSanitizer for C code and exercise filesystem migrations, undo/redo, BASIC parsing and execution limits, independent terminal state, paths, completion, scripts, and host exchange/locking. QEMU tests cover foundational boot/storage behavior, native faults and privileged operations, syscall bounds, watchdog recovery, independent app instances, BASIC/native execution, Paint history, and a complete session reboot. A further QEMU test types through the emulated PS/2 keyboard, opens Terminal through the launcher, and verifies BASIC INKEY during execution. Disk waits collect input without running app actions. Each QEMU test uses disposable images.
