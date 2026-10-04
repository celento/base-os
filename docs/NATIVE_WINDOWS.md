# Independent native primary windows: qualified contract

This records the independent-window contract qualified on the held production
runtime `6d19b75a2caa546be5d86e7e2984a7174fa3265e` on 4 October 2026.
[Exact qualification](NATIVE_WINDOWS_QUALIFICATION.md) records nineteen passed
held-runtime guest phases plus one older-kernel refusal phase. The separate
clean integration `9836220` has equal runtime source and passed 355 host tests
and a clean build; the held guest results are not relabelled as that new binary.
This is not a main-integration or release availability claim. The preceding
[hosted app-view extraction](APP_VIEW_QUALIFICATION.md) remains a distinct,
earlier qualification.

The increment gives a GUI-declared BEX2 process one ordinary decorated primary
window with its own copied title, canvas and bounded Output log. No Terminal is
created or borrowed. It does not provide arbitrary app-created windows, multiple
windows per process, detach/reparent, a heap, larger canvases, app-provided
callbacks, keyboard events, or a graceful-close/save handshake.

## Coordinated additive allocations

The preceding qualified baseline used general ABI 1.1 and independently negotiated UI
1.0. The qualified increment uses general ABI **1.2** and UI **1.1**, with no new syscall:

| Namespace | Allocation |
|---|---|
| BEX2 required flags | `BOS_BEX2_FLAG_NATIVE_WINDOW_V1 = 0x1` |
| Minimum GUI required ABI minor | `BOS_BEX2_NATIVE_WINDOW_ABI_MINOR = 2` |
| General current-context feature | `BOS_FEATURE_OWNED_NATIVE_WINDOW = 1u << 7` |
| UI gateway 29 local operation | `BOS_UI_WINDOW_ADOPT = 6` |
| UI target kind | `BOS_UI_KIND_OWNED_WINDOW = 2` |
| UI capabilities | `BOS_UI_CAP_OWNED_WINDOW = 1u << 8`, `BOS_UI_CAP_FORCED_CLOSE = 1u << 9` |

Calls 0–29 keep their numbers. General query is 96 bytes, memory info 128 bytes,
UI query 64 bytes, and target/event records 96 bytes. BEX2 header remains 64
bytes, format version 1; BEX1 remains unchanged. Process, file, operation and UI
target domains are unchanged, including UI target domain `0x40000000`.
No new public window handle or private built-in-save owner domain is allocated.

With trusted input hooks available, hosted bound tasks advertise existing bit 6
and hosted UI capabilities; owned bound tasks advertise bit 7 and owned UI
capabilities. These bits describe the current attachment, not a global promise
of free windows. An owned task does not advertise HOST_OPEN by setting bit 6.
BEX2 bit 5 continues to describe the running process's memory format. Neither
UI backend is offered to synchronous `exec` or BASIC.

## Build and launch

Build the separate Pointer GUI example with:

```sh
python3 tools/build_app.py examples/c/pointer.c build/pointer-window.bex \
  --format bex2 --window native-v1 --workspace-bytes 0 --stack-bytes 16384
```

The builder and linker emit the required flag and ABI minor 2 together. An
explicit newer minor is permitted; a runtime with an older minor must refuse it.
BEX1 plus `--window`, an unknown window mode, or native-v1 with a required minor
below 2 is rejected. The loader rejects unknown required bits and a flag/backend
mismatch before process allocation. The legacy pure planner entry point still
accepts no flags; the desktop loader supplies an explicit accepted-flags mask.
The required flag is never silently downgraded to hosted operation.

Default Pointer builds remain hosted. `pointer_backend.h` changes only backend
negotiation, endpoint open/reopen and kind/capability validation for the GUI
build. Its drawing model, event consumer, cancellation, signed coordinates and
wait loop are shared. Producer regression tests retain the frozen default BEX1
and unflagged BEX2 Pointer bytes and the unflagged BEX2 minor-1 requirement.
The example installer adds `/Programs/pointer-window.bex` only when
that name is absent, alongside `/Programs/pointer.bex`.

| Input and context | Dispatch |
|---|---|
| BEX1 or unflagged BEX2 from Files/Launcher/Open | Existing hosted Terminal route |
| GUI-flagged BEX2 from Files/Launcher/Open | One independent native primary window |
| BEX1 or unflagged BEX2 from Terminal `start` | Existing task attachment to that Terminal |
| GUI-flagged BEX2 from Terminal `start` | New owned window; calling Terminal remains a shell |
| Any BEX2 through synchronous `exec` | Existing unsupported-context refusal |

Launch validates file identity/type, executable plan, mode and bounded startup
argument. Desktop admission checks trusted input readiness, WM ordering-token
capacity and a free slot. It prepares an invisible `Win` locally, then creates,
binds and starts the owned process without polling or scheduling between these
steps. Only success commits window state, publishes the taskbar entry and focuses
once. No Terminal initialization or selection is part of the owned transaction.
Failure reaps any unscheduled created process and leaves previous window/view
state intact. Failures are reported in the calling Terminal or desktop status;
there is no eviction or hidden hosted fallback. The eight-window bound includes
built-ins and retained result windows.

Older BEX2-enabled kernels reject the nonzero required flag as unsupported before
allocating a process or its pages. BEX2-disabled kernels also refuse before
allocation; a BEX1-only loader rejects the different magic. Older Files/Launcher
code may still open a diagnostic Terminal before discovering that refusal. The
compatibility promise is no GUI app execution or process allocation on that
older kernel, not absence of an old desktop's diagnostic window.

## Adopt the already-owned window

`bos_ui_window_adopt` uses the existing OPEN register shape:

| Register | Value |
|---|---|
| EAX | 29 (UI gateway) |
| EBX | 6 (WINDOW_ADOPT) |
| ECX | requested UI/event major, currently 1 |
| EDX | target-info output offset |
| ESI | declared output capacity |
| EDI | subscriptions; POINTER required, HOVER/WHEEL optional |

It subscribes to the caller's existing primary window; it does not create a
window, allocate pixels, accept a WM slot from the app, or transfer ownership.
Success returns kind 2 and owned/forced-close capabilities in the unchanged
96-byte target record. Common pointer, hover, wheel, implicit capture, bounded
wait and legacy-key-readiness capabilities keep their existing meanings.

- HOST_OPEN from an owned task and ADOPT from a hosted task return UNSUPPORTED.
- A second live endpoint for the same process returns BUSY. Exhausted target or
  identity capacity returns CAPACITY; stale/revoked bindings return STALE.
- Unsupported major returns UNSUPPORTED. Missing POINTER or unknown subscription
  bits return INVALID. The full declared output span is checked before effects.
- Successful ADOPT copies exactly 96 bytes and preserves unused tails. Failure
  leaves output untouched. QUERY still supports a 16-byte prefix of its 64-byte
  structure. INFO/READ/WAIT/RELEASE retain their existing rules.

ADOPT reuses the same eight-endpoint, 64-event-per-endpoint engine, nonwrapping
identities, open fence, initial STATE_RESET, held-button suppression, capture,
geometry/stream epochs, loss handling and reset latch as HOST_OPEN. Consume the
initial reset before a gesture. RELEASE removes only the endpoint: the window,
canvas, log and process remain. Drawing, printing and byte keys continue; another
ADOPT obtains a fresh target identity and reset. The loader never auto-adopts.

## Display, pointer geometry and Output

The internal `WK_NATIVE` kind is appended after all persisted built-in kinds.
Its chrome uses the ordinary 32-pixel titlebar, a 30-pixel Output toolbar and
8-pixel client padding inside the one-pixel border. Native layout fits and
centers the **published** 160×100 or 320×200 canvas into that remaining client
rectangle using widened integer arithmetic and floor rounding. Small windows
can downscale fractionally. Resizing a window does not change logical pixels.
The Terminal layout, including its reserved text area, remains unchanged.
Before the first publication, the trusted snapshot reports zero logical and
viewport dimensions, even if a working canvas has already been resized. This
prevents unpublished working geometry from leaking into target metadata.

`native_canvas_geometry` supplies drawing, snapshots and pointer hit testing.
Letterboxing, padding, toolbar, titlebar, taskbar and another window's exposed
area are not app pointer input. Half-open edges and unclamped captured negative
coordinates retain the hosted contract. Minimize keeps the process running and
its published frame intact while making the target unavailable. Publication
refreshes geometry before later routed input, even when painting is deferred.
The initial owned path uses conservative whole-window composition; the existing
hosted canvas-only partial-render path remains separate.

`bos_write`/print output is copied immediately to an explicit owned log, never
to the selected Terminal. The existing write syscall normalizes nonprinting
bytes and divides output into rows of at most 80 characters. Each view keeps
48 rows of 81 bytes plus head/count/dropped counters: **3,900 bytes**. Overflow
discards the oldest rows and saturates the dropped-row counter; `Output*` and
an "Older output discarded" status expose that loss. No text pointer survives
the callback and no dynamic output allocation is used.

The Output button toggles a scrollable log in place of the canvas. The canvas
and published transform remain intact. While Output is shown the endpoint is
blocked/unavailable, capture is cancelled and held buttons remain suppressed;
the toggling click cannot become an app gesture. Narrow windows reflow rows.
Wheel and Page Up/Down scroll the log. Output changes while it is hidden do not
force a canvas repaint. Ordinary byte keys still go to the live process, after
shell accelerators and modal handling. Ctrl+C stops; Escape is a byte key for
the app, not a shell close action.

Copied program/document names drive the titlebar, taskbar and Monitor window
list. Monitor Tasks includes live hosted and owned processes, with the full
process handle guarding Show/Stop. Window Close separately guards the window
incarnation. Retained result windows remain in Windows but not in live Tasks.

## Publication and lifetime

Process, view and WM slot are separate owners. The process owns its image/pages,
registers, x87 state, waits, keys, copied argument and file/sync/UI resources.
The app view owns the exact process/slot/generation binding, copied title,
canvas, output policy and diagnostic result. The WM owns chrome and z-order.
A view slot is currently the associated WM slot; this is not a public identity.
Generation survives reset/close/reuse and fails closed on exhaustion. Call 12
keeps the explicitly bound one-based display slot, never a process-record index.

Drawing and working resize remain unpublished until the existing boundary:

| Operation | Publication |
|---|---|
| Present, yield, sleep, explicit APP exit (including nonzero) | Existing complete-frame publication |
| Positive 1–60,000 ms native SYNC_WAIT | Only if still PENDING and actually suspending |
| Zero-time sync poll, immediate sync completion/error, wait resumption | None |
| UI WAIT, whether ready, pending, timed out or stale | None |
| Timer preemption, generic error, Stop request, forced Close request | None |

Stop/Close revoke UI eligibility immediately. An already-active slice retains
its ordinary output and explicit publication boundaries until it returns,
including pending positive SYNC_WAIT. APP exit recorded in that slice retains
its result and APP reason after deferred Stop, including negative values such
as −4. Stop itself never publishes, and no additional slice follows the pending
stop. This preserves the qualified extraction semantics rather than imposing
an early output cutoff at UI revocation.

Kernel context is restored before owned backing and file/sync/UI resources are
released exactly once. The common app-view poller drains DONE before scheduling
another peer and schedules at most one slice per turn. Completed owners are
reaped before view results are exposed; stale callbacks cannot write a retained
or reused view.

| Completion/action | Owned-window disposition |
|---|---|
| Explicit APP exit value 0 | Completion requests automatic window close |
| Explicit APP exit nonzero | Retain copied title, bounded log and last published frame |
| Stop or generic error | Retain copied title, bounded log and last published frame; discard unpublished work |
| User Close | Forced termination and dismissal; if active, defer dismissal until cleanup |

A retained result includes exact signed value/reason and lifetime, but its
process identity is diagnostic only. It holds no process, image pages, keys,
waits, file handles, sync interests or UI endpoint. It still consumes a WM/view
slot until dismissed. When no frame exists, Output opens automatically. Close
has no dirty/save veto or lifecycle event; apps must save explicitly before
exit. A shared durable commit already underway follows the existing coordinator
rules and is not cancelled merely because one owner's interest is released.

Session save and restore explicitly accept only persisted built-in kinds and
skip `WK_NATIVE`. Neither an active app nor a retained result is restored after
reboot. No session wire layout or built-in kind number changes.

## Measured i386 storage and stack review

Compile-only probes use the actual 32-bit freestanding definitions, not host
pointer sizes. The frozen 6d19b75 i386 probe reports:

| Typed allocation | Bytes |
|---|---:|
| Terminal text, one slot | 27,432 |
| View metadata before canvas, one slot | 112 |
| Working canvas and publication metadata, one slot | 64,032 |
| Complete app view, one slot | 64,144 |
| Output log, one slot | 3,900 |
| Eight text records, views and logs (`AppStorage`) | 763,808 |
| Original Terminal container | 786,432 |
| Remaining container space | 22,624 |
| Published pixels / original published arena | 512,000 / 524,288 |
| Remaining published arena space | 12,288 |
| Process record / eight records | 4,720 / 37,760 |
| UI target / eight target records | 6,364 / 50,912 |
| Ordered input ingress / its sample array | 16,456 / 16,384 |

`AppStorage` stays at `APPS_BASE + 0x300000`; published pixels remain at
`NATIVE_CANVAS_BASE`. The extra log and metadata use **31,520 bytes** of the
qualified extraction's existing spare container, with **zero additional surface
pages**. The earlier design-only 776,192-byte budget is not this measured layout.
Eight process records end at `0x03009380`, leaving 552,064 bytes before
`TASK_PAGE_METADATA_BASE=0x03090000`. Static assertions guard these boundaries.
GUI process code/data/stack still require their normal BEX2 committed pages;
zero surface pages does not mean a zero-page process. The current separate GUI
Pointer artifact is 12,288 file bytes with 4,660 text bytes, 32,128 data-memory
bytes, no workspace and a 16,384-byte stack. Its rounded commitment is 2 text +
8 data + 4 stack + 2 page-table/directory pages = **16 owned pages**. These
artifact-specific sizes are not a new executable policy or a final kernel hash.

Separate, serial compilation of `kernel`, `term`, `app_view`, `app_canvas`,
`app_storage`, `canvas_view`, `process`, `native_ui`, `executable`, `sysmon` and
`input_ingress`
with GCC 14.2.0 and Makefile production CFLAGS plus `-fstack-usage` gives these
per-function frames (bytes include GCC's bounded outgoing-call usage):

| Function/path component | Bytes |
|---|---:|
| `native_window_start` / `terminal_native_launch` | 144 / 64 |
| `app_view_start_owned_file` / `process_create_mode` | 144 / 240 |
| `process_probe_launch` / `image_plan` / executable flag planner | 128 / 96 / 128 |
| `draw_native_window` / `draw_app_canvas` | 240 / 80 |
| `native_canvas_geometry` / native layout / integer divide helper | 144 / 48 / 48 |
| `native_host_snapshot` / endpoint `open_kind` | 96 / 128 |
| `output_write` / owned result consumer / `app_view_title` | 64 / 176 / 144 |
| `app_view_poll_update` / `app_view_close` / `app_view_stop` | 64 / 64 / 64 |
| `process_interrupt` (inlines query/UI dispatch) | 256 |
| `sysinfo_fill` / `sysmon_draw` / `sysmon_click` | 1,120 / 480 / 240 |
| `draw_one_window` (includes inlined window content) | 2,080 |
| Existing Terminal `execute` / `term_enter` | 5,888 / 144 |
| Existing session save / restore | 13,024 / 13,008 |

The exact frozen-source review contains 423 function reports, all static or
dynamic-bounded; none is reported unbounded. The kernel stack reservation is unchanged at 65,536 bytes,
`[0x1F0000,0x200000)`. The largest individual measured frame leaves 52,512 bytes
before callees. Existing Terminal scripts permit five nested `execute` frames
(depth 0–4): these alone sum to 29,440 bytes. Adding the measured `kmain`,
`handle_key`, `term_enter`, launch adapter, owned launcher/start, process-create,
image-plan and executable-planner frames gives a conservative selected-chain
sum of 30,832 bytes, leaving 34,704 bytes for other callers/callees and interrupt
frames. This sum is **not** a whole-program worst-case or runtime stack high-water
proof. The separate syscall/interrupt stack also keeps its 65,536-byte reservation
at `TASK_INTERRUPT_STACK_BASE`; BEX2 user stacks are distinct again.

The final held 6d19b75 ELF ends at `0x1B1A90`, with **255,344 bytes** before
`STACK_BOTTOM`; the clean 9836220 integration ELF independently has the same end.
Their artifact hashes differ and are recorded separately in the
[qualification](NATIVE_WINDOWS_QUALIFICATION.md#exact-source-and-build-identities).
The linker retains `__kernel_end <= STACK_BOTTOM`. The code/BSS gap is not counted
as part of the reserved kernel stack. UI targets occupy 50,912 bytes of a
54 KiB cap, leaving 4,384 bytes; ordered ingress remains 16,456 bytes.

The budget review itself was compile-only. The separately executed ordinary
guest qualification and clean host/build results are recorded in
[NATIVE_WINDOWS_QUALIFICATION.md](NATIVE_WINDOWS_QUALIFICATION.md), including
unsuccessful observer/provenance attempts and their corrected reruns. No debugger,
guest-memory inspection, fuzzing or intentional-fault route was used.
