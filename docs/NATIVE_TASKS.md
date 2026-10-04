# Protected native desktop tasks

`start /Programs/counter.bex` starts an ordinary BEX1 program without the two-second lifetime limit of `exec`. Open another Terminal and run the same command: each copy maintains its own counter, canvas, input, stack, and data. The desktop continues taking input and servicing its cooperative built-in applications between native slices.

BEX1 remains the default 64 KiB contract. The same hosted desktop route also
supports the qualified opt-in [BEX2 format](BEX2_FORMAT.md), whose sparse 4 MiB
offset extent contains only launch-declared text/data/workspace/stack regions.
Synchronous `exec` still accepts BEX1 only. A BEX2 explicitly built with
`--window native-v1` uses the qualified [owned native window](NATIVE_WINDOWS.md)
backend instead of a Terminal. Neither format supplies a heap.

Open `.bex` programs from Files, the ordinary Open dialog, or Ctrl+Space filename
search. Each activation gets a fresh hosted or owned view and the same protected
runtime as `start`; no Terminal command is required. A full eight-window desktop
asks for a window to be closed. Owned launch is transactional and never consumes
the calling Terminal on failure; errors are visible without a hidden hosted
fallback. Unsupported binaries are never opened as text or modified. Files/Open/search reject a stale
node incarnation before activation; the loader copies the current program.

## Controls and persistence

- `start FILE [DOCUMENT]` hosts an unflagged app in this Terminal, or launches a
  GUI-flagged BEX2 in its own window while keeping the calling shell. Quote paths with
  spaces; the optional ordinary-file path is resolved to an absolute path of at
  most 128 printable ASCII bytes. Starting over a live task is rejected.
- Printable keys, Enter, Backspace, and Escape are queued for the focused task. Ctrl+C is reserved for stopping it through the desktop.
- `tasks` lists the display/view slots with runnable or sleeping tasks. `stop` stops the current terminal's task (also usable in a command script).
- Minimizing or switching away leaves a task running. Closing or resetting its Terminal cancels it. A reused window slot starts clean.
- A native exit or fault ends only that task. Stop prevents another slice. An
  already-active slice keeps its established output/publication and APP-exit
  precedence until return; see [lifetime ordering](PROCESS_LIFETIME.md).
- Owned APP exit zero closes its window. Stop/error/nonzero exit retain an inert
  result with Output and the last published frame. Current Close is forced;
  native private buffers and result windows are not saved into session-v1.
- The desktop session does not save live executable state. Apps explicitly save documents through the file API. Counter uses `/Documents/counter-N.txt`, where N is its terminal slot; S writes and explicitly synchronizes the disk; a later start in that slot loads the saved number. A failed sync says RAM only instead of Saved. Notebook follows the same explicit-sync rule.
- The legacy task-id API still identifies a reusable display slot. The additive
  [platform query](NATIVE_PLATFORM_ABI.md) provides a separate nonreused process
  identity; owned file/completion resources are released on exit or close.

## Execution model

There are eight independently allocated process records. Each holds its memory
plan and owned-page references, a complete ring-3 interrupt frame, x87 state,
copied contextual output callbacks, a 32-byte key queue, and wait/deadline state.
BEX1 owns sixteen backing frames for its 64 KiB image; BEX2 owns its declared
regions and private page-directory/table frames. Record indices are neither
process handles nor display slots. Each common app view retains an exact opaque
process handle and nonwrapping binding generation. The supervisor-only records
at `TASK_BASE` (48 MiB) fit in the reserved 1 MiB arena; owned frames come from
the separately validated page pool.

A process-table round-robin poll selects the next runnable record. BEX1 gathers
its sixteen backing frames into the compatibility `USER_BASE` region before
entry and scatters the complete 64 KiB back once after a continuing slice.
BEX2 switches to its private root and descriptors without per-slice image
copying. Both restore registers/x87 and resume with IRET. A PIT interrupt returns
to the desktop after at most one tick of uninterrupted user execution (about
14.3 ms at 70 Hz); explicit present/yield/sleep or a pending operation wait can
return earlier. Saved EIP is after the completed syscall, so filesystem
operations are not replayed. On return, both restore the kernel root/descriptors,
then FPU context, before clearing active state and copying or releasing backing.
An exiting BEX1 skips the final scatter. Both formats retain identity and result
until the common app-view consumer consumes completion and reaps the record.

Only ring-3 execution is preempted. A syscall completes atomically on the kernel's bounded exception stack. Syscall buffer checks, transfer limits, canvas clipping, and `/Documents` write restrictions are shared with legacy `exec`. No kernel task, filesystem operation, or GUI handler is interrupted by another native task. A task can be delayed by rendering, disk access, BASIC, synchronous `exec`, or other cooperative work. This provides responsive bounded native slices, not real-time guarantees or a general-purpose kernel scheduler.

Owned operation waits use the existing sleeping state. They store only an opaque
operation handle and bounded deadline, poll without driving disk I/O, and resume
with a terminal result or explicit timeout. Stop/close releases resources even
while waiting; input remains queued. Ordinary sleep semantics are unchanged.

Tasks have no cumulative runtime limit. A CPU-bound loop is repeatedly preempted and can be canceled from the desktop. No arbitrary kernel stack is kept suspended across a yield. The only resumed stacks belong to the isolated user image.

## Added ABI

Existing BEX1 headers and syscalls 0–9 retain their argument and return contracts.
BEX1 executable files are at most 49,152 bytes, with IDE storage required above
the legacy floppy limit of 16,383 bytes. BEX1 code, BSS and stack share 64 KiB;
its SDK linker reserves at least 16 KiB for the stack. BEX2's separate file and
region limits are defined in [BEX2_FORMAT.md](BEX2_FORMAT.md).

| Syscall | SDK wrapper | Meaning |
| --- | --- | --- |
| 5 | `bos_present()` | In task mode, publishes the complete canvas then yields. In `exec`, calls the legacy presenter. |
| 10 | `bos_yield()` | Publish pending canvas and resume on a later desktop turn; returns 0. `exec` returns -1. |
| 11 | `bos_sleep(ms)` | Publish pending canvas, then wait at least the requested 0–60,000 ms, rounded up to PIT ticks. Zero yields. Invalid values and `exec` return -1. |
| 12 | `bos_task_id()` | Owning display/view slot 1–8 (Terminal for hosted apps); `exec` returns 0. |
| 13 | `bos_read_file_at(path,out,capacity,offset)` | Up to 4096 bytes from any file offset; returns 0 at/beyond EOF. |
| 14 | `bos_canvas_size(width,height)` | Select and clear exactly 160×100 or 320×200; new apps default to 160×100. |
| 15 | `bos_replace_file(path,data,bytes)` | Atomic RAM replacement up to 32,768 bytes, confined to `/Documents`. |
| 16 | `bos_sync()` | Durable filesystem snapshot result: 0 success, -1 failure. |
| 17 | `bos_argument(out,capacity)` | Copy the optional startup path plus NUL; return byte length, 0 absent, or -1. Capacity 0 queries length. |

The extended calls do not enlarge application memory or bypass filesystem
capacity. Sync can delay scheduling while the bounded disk write completes.
The [native SDK guide](NATIVE_SDK.md) details streaming consistency, replacement
failure behavior, canvas callback integration and the DocStats C example.

Key polling is nonblocking and returns one queued byte or zero. A full queue drops the newest key. Keys remain queued through sleep; input does not shorten the sleep deadline. No key is shared with another owner, and stop/restart clears pending input. General registers other than syscall EAX and x87 state survive a returning syscall or timer slice. The standard SDK targets software floating point with SSE/MMX disabled; x87 task state is explicitly isolated.

## Kernel integration

- `term_task_start_file(slot, file, identity)` remains the no-argument desktop
  route; `term_task_start_file_with_arg` adds copied document launch metadata.
  Both reject changed/non-file identities and busy owners, preserve the caller's
  selection, and only clear the canvas after an accepted start.
- `process_create(file, bytes, argument, length, out_process)` validates/copies
  the BEX1 image or validated BEX2 regions and optional path into an independent
  CREATED record. The output handle remains untouched on failure. It is not
  yet runnable.
- `process_bind` copies a `ProcessIO` table with `(process, slot, generation)`;
  `process_start` commits READY. Unbinding a CREATED record does not stop it or
  release resources. No public detach/reassignment/background launch is added.
- `process_schedule_one()` selects at most one process-table slice. It never runs
  in IRQ/device polling. `process_step(handle)` also resolves an exact handle.
- `process_status`, `process_get_result`, `process_key`, `process_request_stop`
  and `process_reap` use full nonreused owner handles. Reaping is idempotent; a
  stale handle cannot stop or target the current occupant of a reused record.
  Application exit (including -4) remains distinct from a requested Stop.
- `term_task_poll_update()` translates that process's explicit attachment into a
  display update, consumes DONE, and reaps. It never selects a Terminal to make
  output callbacks work. A callback validates all attachment fields and updates
  only its explicit Terminal; stale callbacks are discarded.
- `term_task_running/key/stop/close` route focus, Ctrl+C and close by explicit window slot. Reset also clears the owned task.
- `ProcessIO.resize` is optional. Terminal supplies it; accepted mode changes clear
  working pixels. A task retains its active dimensions across slices. The renderer
  uses `term_canvas_width/height`; pixels are packed with the published width.
- `ProcessIO.present` publishes complete working pixels plus geometry before
  explicit present/yield/sleep and explicit application exit (any exit status).
  A positive `bos_sync_wait` (1–60,000 ms) also publishes only when its operation
  is pending and it actually suspends; polling and immediate completion/error do
  not. `bos_ui_wait`, timer preemption, Stop and generic failure never publish.
  Terminal retains the last published frame until reset/new launch, so a full
  redraw cannot reveal unfinished working pixels. This callback does not dispatch
  applications.
- Published canvases occupy 512,000 bytes of the 512 KiB reservation at
  `0x600000`–`0x680000`, inside the existing E820-validated RAM span.
- Historical C1 footprint: eight Terminals with 320 scrollback rows, maximum
  320×200 canvases and binding generation used 732,160 bytes. Later input fields
  changed that figure; a compile-time assertion still enforces the fixed
  786,432-byte subarena before script scratch.

The legacy synchronous `process_run` still has its two-second watchdog, checked syscalls, and audio pause/resume behavior. Running it does not destroy saved task images; asynchronous tasks resume afterward.

## Verification

`python3 tools/task_test.py build` creates a disposable disk and boots an actual 64 MiB QEMU guest. It checks:

- Two native C apps progress independently for more than three seconds
- Sleep deadlines, yield, isolated keys, queue overflow/wrap, and per-owner file effects
- An uncooperative CPU-bound loop returns at a PIT slice and does not starve the two apps
- Two different live x87 stacks plus the kernel's x87 state survive interleaving; a fresh task gets cleared x87 register storage
- Graceful exit, cancellation, restart, terminal selection, close and reset
- The legacy two-second watchdog still terminates CPU-bound `exec`, without changing saved task images
- A legacy synchronous C app still reads/writes its persistent document

The test does not use intentional invalid instructions, privilege violations, malformed executables, or memory-fault probes. Those are separate from normal runtime/lifecycle coverage.

`python3 tools/task_media_test.py build` additionally creates an original four-second
MP3 fixture with ffmpeg, serves a loopback HTTP page, and boots QEMU with real
RTL8139 and SB16 devices. Two counter apps and two x87 probes run while the browser
cancels a slow request, loads its replacement, and the desktop redraws. It checks
independent counter saves, complete MP3 playback with no underruns, and captures
both a screenshot and non-silent PCM output. This is a focused subsystem smoke;
the main desktop must still wire its real focus/close/poll events as described above.

`python3 tools/native_sdk_test.py build` verifies exact 48 KiB images, two
concurrent 2 MiB streams, EOF and partial reads, independent larger canvases,
32 KiB durable replacement, DocStats sample/empty counts, legacy mode resets and
full-volume rollback across reboot. Its host companion is `test_native_sdk.py`.

The startup argument is immutable launch text, not a file handle or filesystem
snapshot. `term_task_info` returns copied program and document basenames plus the
existing task instance guard. `term_task_title` combines those names without
changing selection; it returns no live title after exit, stop, close or reset.
`tasks` prints the copied names alongside each live owner. See the SDK's startup
argument section for exact query/copy and legacy behavior.

The synchronous BASIC/`exec` path retains its existing `ProgramIO` adapter.
`ProcessCounts` is an internal bounded diagnostic (records, created, live,
exiting, done, and resource-owning records), not a new syscall or memory promise.
See [process lifetime notes](PROCESS_LIFETIME.md) for ordering and release gates.
