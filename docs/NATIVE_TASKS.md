# Protected native desktop tasks

`start /Programs/counter.bex` starts an ordinary BEX1 program without the two-second lifetime limit of `exec`. Open another Terminal and run the same command: each copy maintains its own counter, canvas, input, stack, and data. The desktop continues taking input and servicing its cooperative built-in applications between native slices.

## Controls and persistence

- `start FILE` starts one task owned by this Terminal. Starting over a live task is rejected.
- Printable keys, Enter, Backspace, and Escape are queued for the focused task. Ctrl+C is reserved for stopping it through the desktop.
- `tasks` lists the terminal slots with runnable or sleeping tasks. `stop` stops the current terminal's task (also usable in a command script).
- Minimizing or switching away leaves a task running. Closing or resetting its Terminal cancels it. A reused window slot starts clean.
- A native exit or fault ends only that task. A canceled task does not receive another slice or perform another syscall.
- The desktop session does not save live executable state. Apps explicitly save documents through the file API. Counter uses `/Documents/counter-N.txt`, where N is its terminal slot; S saves and a later start in that slot loads the saved number.
- Runtime ownership is per slot, not a durable process identity. The same slot may be reused after close.

## Execution model

There are eight fixed owner slots. Each stores a 64 KiB image, a complete ring-3 interrupt frame, x87 state, copied output callbacks, a 32-byte key queue, and a sleep deadline. These live in supervisor-only memory at `TASK_BASE` (48 MiB); all eight fit within the 1 MiB reserved arena. Boot memory validation must cover the arena; the QEMU task tests use 64 MiB.

Only one image is mapped into `USER_BASE` at a time. A desktop poll selects the next runnable owner fairly, copies its image into the protected region, restores its registers and x87 state, and resumes with IRET. A PIT interrupt returns to the desktop after at most one tick of uninterrupted user execution (about 14.3 ms at 70 Hz). `present`, `yield`, and `sleep` can return earlier. The saved EIP is the instruction after a completed syscall; filesystem operations are not replayed on resume. Before another app runs, the outgoing image and FPU state are saved and the kernel FPU state restored.

Only ring-3 execution is preempted. A syscall completes atomically on the kernel's bounded exception stack. Syscall buffer checks, transfer limits, canvas clipping, and `/Documents` write restrictions are shared with legacy `exec`. No kernel task, filesystem operation, or GUI handler is interrupted by another native task. A task can be delayed by rendering, disk access, BASIC, synchronous `exec`, or other cooperative work. This provides responsive bounded native slices, not real-time guarantees or a general-purpose kernel scheduler.

Tasks have no cumulative runtime limit. A CPU-bound loop is repeatedly preempted and can be canceled from the desktop. No arbitrary kernel stack is kept suspended across a yield. The only resumed stacks belong to the isolated user image.

## Added ABI

Existing BEX1 headers and syscalls 0–9 retain their argument and return contracts. Executable file size is at most 49,152 bytes, with IDE storage required above the legacy floppy limit of 16,383 bytes. Code, BSS, and stack still share 64 KiB. The SDK linker reserves at least 16 KiB for the stack.

| Syscall | SDK wrapper | Meaning |
| --- | --- | --- |
| 5 | `bos_present()` | In task mode, yields after drawing. In `exec`, calls the legacy presenter. |
| 10 | `bos_yield()` | Resume on a later desktop turn; returns 0. `exec` returns -1. |
| 11 | `bos_sleep(ms)` | Wait at least the requested 0–60,000 ms, rounded up to PIT ticks. Zero yields. Invalid values and `exec` return -1. |
| 12 | `bos_task_id()` | Owning terminal slot 1–8; `exec` returns 0. |
| 13 | `bos_read_file_at(path,out,capacity,offset)` | Up to 4096 bytes from any file offset; returns 0 at/beyond EOF. |
| 14 | `bos_canvas_size(width,height)` | Select and clear exactly 160×100 or 320×200; new apps default to 160×100. |
| 15 | `bos_replace_file(path,data,bytes)` | Atomic RAM replacement up to 32,768 bytes, confined to `/Documents`. |
| 16 | `bos_sync()` | Durable filesystem snapshot result: 0 success, -1 failure. |

The extended calls do not enlarge application memory or bypass filesystem
capacity. Sync can delay scheduling while the bounded disk write completes.
The [native SDK guide](NATIVE_SDK.md) details streaming consistency, replacement
failure behavior, canvas callback integration and the DocStats C example.

Key polling is nonblocking and returns one queued byte or zero. A full queue drops the newest key. Keys remain queued through sleep; input does not shorten the sleep deadline. No key is shared with another owner, and stop/restart clears pending input. General registers other than syscall EAX and x87 state survive a returning syscall or timer slice. The standard SDK targets software floating point with SSE/MMX disabled; x87 task state is explicitly isolated.

## Kernel integration

- `process_task_start(owner, file, size, io)` validates and copies the BEX1 plus callback values. It never retains a pointer into a mutable filesystem node.
- `process_task_step(owner)` runs at most one user slice, skipping a sleeper until its deadline. It is called only from the desktop's normal context, never an interrupt or reentrant polling hook.
- `process_task_status/result`, `process_task_key`, `process_task_stop`, and `process_task_clear` expose bounded lifecycle operations.
- `term_task_poll()` selects one fair runnable terminal, preserves the caller's selected terminal, and returns whether its output changed. The desktop marks itself dirty when this returns true.
- `term_task_running/key/stop/close` route focus, Ctrl+C and close by explicit window slot. Reset also clears the owned task.
- `ProgramIO.resize` is optional. Terminal supplies it; accepted mode changes clear
  pixels. A task retains its active dimensions across slices. The renderer must
  use `term_canvas_width/height`; pixels are packed with the current width.
- Eight Terminals with 320 scrollback rows and maximum 320×200 canvases use
  731,744 bytes, below the fixed 786,432-byte subarena before script scratch.

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
