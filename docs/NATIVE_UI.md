# Hosted native pointer service

The current qualified hosted-pointer contract provides one kernel-owned endpoint
for an existing native Terminal canvas. The general [platform ABI is 1.1](NATIVE_PLATFORM_ABI.md);
UI **1.0** is negotiated independently through gateway **29**. Hosted-UI feature
bit **6** is advertised only for bound desktop tasks when trusted production
input hooks are available. Synchronous `exec` and BASIC do not offer this service.
See [isolated qualification](NATIVE_UI_QUALIFICATION.md) and its linked integration
evidence for the tested source and scope; the staged history below is not a
current instruction to leave the gateway disabled.

Existing calls 0–27, byte-key queues, and forced Stop/host-close remain intact.
Memory-info retains call 28 and feature bit 4; current-context BEX2 retains bit 5.
The general ABI query remains 96 bytes. UI targets use a private allocation
domain (`0x40000000`); applications treat handles as opaque. This service creates
no windows or surfaces and adds no keyboard-event or graceful-close protocol.
Independent app views/windows remain separate, unmerged work.

## Current client quickstart

Build `examples/c/pointer.c` with the normal SDK (default BEX1, or explicit
`--format bex2`) and launch it through Files, Open, the launcher or Terminal
`start`. On older or unavailable runtimes it reports unsupported normally.

1. Check `bos_abi_query` and `BOS_FEATURE_HOSTED_UI`, then negotiate
   `bos_ui_query` before reading its output. Open with `bos_ui_host_open` and
   required `BOS_UI_SUB_POINTER`; HOVER and WHEEL are optional subscriptions.
2. Consume the mandatory OPEN STATE_RESET. Drain `bos_ui_read` and the existing
   `bos_key` byte queue; READ events keep their historical coordinates/geometry.
   RESET/CANCEL abort a local gesture. Only ordinary final UP commits Pointer's
   preview, and captured coordinates may lie outside the logical canvas.
3. Explicitly present each complete dirty frame, then use `bos_ui_wait` with
   QUEUE, optionally LEGACY_KEY. UI WAIT never publishes a working frame.
4. Release the endpoint when finished; reopen gets a fresh identity and does not
   turn an already-held button into a new press. Host close still stops the task.

## Current wire contract

`sdk/baseos_abi.h` defines fixed four-byte word layouts and checked offsets:
64-byte UI query, 96-byte target info, 96-byte event. All reserved words are zero.
The local operations are QUERY=0, HOST_OPEN=1, INFO=2, READ=3, WAIT=4, RELEASE=5.
The negotiated UI/event major is 1 (UI minor 0). POINTER is required; HOVER and
WHEEL are optional. No window/keyboard-event/surface capability is declared.

Set EAX=29 for the syscall; the five argument registers are EBX, ECX, EDX, ESI,
EDI in that order. EAX receives the result; other registers are preserved:

| Operation | EBX, ECX, EDX, ESI, EDI |
|---|---|
| QUERY | 0, requested major, output, capacity, 0 |
| HOST_OPEN | 1, requested major, output, capacity, subscriptions |
| INFO | 2, target, output, capacity, 0 |
| READ | 3, target, output, capacity, 0 |
| WAIT | 4, target, QUEUE optionally with LEGACY_KEY, milliseconds, 0 |
| RELEASE | 5, target, 0, 0, 0 |

The process boundary checks the full declared output span before allocation
or queue consumption. Query copies min(capacity, 64), with a 16-byte minimum;
OPEN/INFO/READ require at least 96 bytes and copy exactly 96. Errors/PENDING leave
all output untouched; successful copies preserve an unused output tail.

- READ returns `BOS_OK` (0) after copying and consuming one 96-byte event (the
  reset latch first), `BOS_PENDING` (1) if empty, or an error. Only success copies.
- WAIT returns `BOS_OK` when a queued event/reset or requested legacy key is ready,
  without consuming it. Zero milliseconds returns `BOS_PENDING` if not ready.
  A pending wait of 1–60,000 ms suspends only this caller; readiness wins a
  same-tick deadline. `BOS_E_TIMEOUT` (-1010) leaves the endpoint alive.
- RELEASE returns `BOS_OK` for the live owned endpoint. A repeated RELEASE, or
  another use of a released, revoked, wrong-owner or stale target, returns
  `BOS_E_STALE` (-1005); discard that handle. Internal owner cleanup remains
  idempotent. Invalid arguments and unavailable contexts retain their documented
  INVALID/UNSUPPORTED errors.

UI WAIT never publishes, retains no user pointer, and cannot satisfy a separate
sync wait. By contrast, a positive pending `bos_sync_wait` publishes only when
it actually suspends; zero-time polls and immediate completion/error do not.
See [all canvas publication boundaries](NATIVE_CANVAS_PUBLICATION.md).

## Historical stage 1: pure geometry and endpoint core

This and the later stage records preserve implementation/verification history.
Current availability, client behavior and wire contract are described above.

At this stage the gateway is reserved but is not dispatched or advertised.
`native_ui.c` can only be used after trusted desktop hooks are configured.
Production process/wait dispatch and the sole ordered desktop route come later,
and require separate host and ordinary PS/2 guest gates before integration.

`canvas_view.c` is the shared source of published-canvas sizing, origin, half-open
containment, and widened mathematical-floor coordinates. Drawing uses the same
layout. Working pixels and successful working resize are not publication. The
100/200/400 height thresholds, left alignment, fractional scaling and trailing
eight-pixel text gap match the existing renderer. Captured coordinates remain
signed outside the viewport; unrepresentable mathematical results saturate only
at the signed 32-bit wire range. Normal screen/window sizes never reach that cap.
The helper does not perform WM ownership or screen clipping.

The endpoint core has eight fixed records, one live endpoint per process,
64 96-byte events per record and a separate mandatory reset latch. The built
stage-1 i386 records occupy 50,848 bytes; the existing ordered ingress occupies
16,456 bytes. Linker assertions keep all initialized data/BSS below the reserved
kernel stack. No application callback or user buffer is retained.

Every endpoint copies process/slot/generation. Fixed trusted snapshot hooks must
check the live process and matching Terminal binding. Each syscall-side owner
operation and each route refresh validates that snapshot. Wrong-owner, released,
revoked and stale handles do not affect another endpoint. Handles, sequence
numbers, geometry tokens and stream tokens do not wrap. Internal owner cleanup
is idempotent; public RELEASE succeeds once and a repeated call returns STALE.
Reopened targets have fresh identities.

Opening fences all already-acquired input, suppresses physically held buttons,
and requires reading the OPEN STATE_RESET before any gesture can begin. Routing
uses observed sample order. DOWN focuses before motion/button; capture retains
the exact target through outside motion, left/right chords and final UP/wheel.
Already-held buttons from a gesture begun elsewhere cannot be imported into a
new capture. A peer's unread reset does not intercept an existing capture.

Cancellation clears accepted buttons and capture before later state/geometry
notifications. Geometry cancellation carries the old transform; subsequent
GEOMETRY carries the new one. Focus, overlay, minimization, publication geometry
and explicit scene barriers cancel without committing an app drag. Authoritative
input-loss/scene-discard barriers resynchronize both physical-button baselines
and suppression so a discarded release cannot swallow the next fresh click.

Only adjacent identical-context MOVE tails coalesce. State/button/wheel records
are barriers. Overflow flushes the ring, cancels capture, increments the stream
token and preserves a retrievable STATE_RESET with a saturating discarded-event
count. A pending reset does not rearm gestures. READ preserves the exact historical
coordinates and dimensions; INFO reports current target state. An outside hover
boundary clears POSITION_VALID once rather than becoming a global mouse monitor.

## Historical executed stage-1 checks

- `test_canvas_view.py`: 1,932 deterministic legacy layouts, viewport edges,
  fractional/negative/offscreen coordinates; i386 has no division-runtime import.
- `test_native_ui.py`: real core under ASan/UBSan; exact button/move/wheel order,
  focus, two targets, implicit outside capture, independent modifiers, copied
  binding/stale-owner rejection, OPEN/reopen fences, preheld chords, geometry and
  minimized/blocked cancellation, hover departure, MOVE coalescing, full queue at
  wheel/UP, reset recovery, authoritative successive scene/input-loss barriers.
- `test_native_render.py`: original production rendering equivalence and guards
  with the shared geometry helper; published owner selection remains independent.
- Freestanding kernel image built successfully. This is not guest qualification.

No fuzzing, deliberate memory faults, debugger routes or guest callbacks are
part of these deterministic host checks.

## Historical stage 2: process boundary and SDK

Gateway 29 is now dispatched only for a bound desktop task when fixed UI hooks
are configured. Synchronous `exec`/BASIC remain unsupported. The ABI feature bit
is conditional on that same service availability. The ordinary kernel does not
configure hooks until the production-routing stage, so this stage alone does not
advertise a usable service.

The SDK provides `bos_ui_query`, `bos_ui_host_open`, `bos_ui_info`, `bos_ui_read`,
`bos_ui_wait`, and `bos_ui_release`. The complete declared output span is checked
before any service effect. A query supports a 16-byte prefix; info/read/open copy
exactly 96 bytes and preserve tails. PENDING/errors leave output untouched.

Process wait kinds are explicitly NONE, TIMER, SYNC and UI. UI waits store only
a target, readiness flags and rounded-up wrap-safe deadline. Readiness wins a
same-tick deadline. QUEUE is mandatory; optional LEGACY_KEY observes without
consuming the existing byte queue. UI events never wake a separate sync wait.
UI WAIT never publishes, including immediate-ready and timeout paths. Explicit
present/yield/sleep/exit retain their pre-existing publication behavior. A pending
positive native SYNC_WAIT also publishes before suspension under its existing
contract; it is distinct from UI WAIT.

Process binding checks now reject a stopping owner immediately; Stop/exit revoke
UI eligibility before deferred backing cleanup. The existing owner-release path
cleans endpoints exactly once after returning to kernel context. No process can
resume a revoked UI wait as ready, even when an old byte key remains.

`test_native_ui.py` additionally links the real core to the production extracted
syscall dispatcher and scheduler. Executed checks cover prefix/tail/no-write
rules, full declared-span rejection before OPEN/READ, major/flag negotiation,
WAIT 0/1/60000, wrap/tie/timeout/retry, byte-key readiness, independent sync wait,
no implicit publication, wrong-owner calls and immediate/deferred Stop/exit.
Existing native-platform, publication-process, process-lifetime and private
address-space host gates also pass. A freestanding kernel build passes. None of
these host results substitute for the still-pending ordinary production guest
routing and unchanged BEX1 compatibility gates.

## Historical stage 3: production routing candidate and C example

The ordinary desktop now configures trusted hooks when PS/2 initialization
succeeds. Native pointer input uses the sole ordered ingress. Device polling still
only acquires records. The front-to-back WM point owner, shell overlays, taskbar,
chrome, padding and the owner's published canvas decide eligibility. A shell
left/right gesture already underway cannot be stolen by a later chord over a
native canvas. Native capture bypasses other windows and remains exclusive until
its accepted buttons are released or a cancellation occurs. Unsubscribed paths
retain the existing shell/built-in handlers.

The snapshot adapter validates both `process_binding_live` and the complete
Terminal incarnation. It reads `term_canvas_size(slot)` without changing the
selected Terminal. Rendering and routing use `canvas_view_layout`. The task
publication boundary is refreshed before later routed samples even when its
paint is occluded/deferred. Changed working size remains private until present.

Cancellation conservatively consumes a held gesture across newly chorded
buttons: after cancelled LEFT, a RIGHT press while LEFT remains held is also
suppressed until all held buttons are released. It does not become a gesture in
another application. A scene-discard/input-loss boundary that authoritatively
observes all buttons released clears the old suppression baseline.

`examples/c/pointer.c` is embedded by the qualified runtime; its normal example
installer creates `/Programs/pointer.bex` when missing and preserves an existing
name. The explicit `--pointer-example` packaging option adds an archived
source-built binary, provenance and `Pointer guide.txt` to fresh release disks,
requiring any already-seeded Pointer to match byte-for-byte. See
[release instructions](RELEASE.md#optional-fresh-disk-pointer-sdk-example).
The option preserves the existing runtime/defaults and never opens saved disks.
It is a regular BEX1 SDK app, also buildable as BEX2. Left/right draw separate ink;
a chord previews both. Only ordinary final UP commits the preview. RESET/CANCEL
aborts it. R toggles 160×100/320×200 with explicit publication; C clears; O
releases/reopens; P prints normal Terminal diagnostics; Q/Esc exits. A separate
model/event adapter keeps window-backend details outside drawing logic. It drains
both sources, presents dirty frames explicitly, then waits with QUEUE|LEGACY_KEY.
No independent window or keyboard-event service is implied.

Additional executed host gates:

- Real WM adapter + ingress + UI core: exposed-point ownership, focus-before-down,
  outside capture across a peer, overlays, geometry publication, legacy point
  exclusion and decoded-input loss reset.
- Direct finite-state unit boundaries: last target serial never reused even after
  initialization, sequence/geometry/stream exhaustion revoke safely, stale output
  remains untouched, and a fresh endpoint can reopen after token exhaustion.
- Real example under ASan/UBSan: chords, cancelled-preview rollback, signed outside
  positions, unknown event/capability bits, resize publication, reopen and bounded
  waits. Both BEX1 and BEX2 builds pass.

Production guest validation and final full-suite results are recorded separately
once run; these host checks alone do not qualify the candidate for integration.

## Historical isolated qualification

The exact runtime at `9e52fad` passed independent source review, all 227
host tests, 41 ordinary screenshot/offline assertions in each default/large
profile, both existing production frame-publication profiles, and the legacy
Files/search launch/save/Stop/exit gate. See [exact qualification evidence](NATIVE_UI_QUALIFICATION.md)
for binary hashes, observed scope and limitations. This does not imply that the
parent integration branch or an hourly package already ships the feature.

## Hosted app-view ownership extraction

The hosted implementation now keeps Terminal command state separate from the
bounded app view. This is an internal prerequisite only: existing BEX1/BEX2
launches still use Terminal, and no window kind, launch flag, ABI value or public
capability has been added.

- `app_canvas` owns explicit working/published canvas metadata and bounded
  drawing/publication helpers. Its frame snapshot contains the visible pixels
  and dimensions together; BASIC/exec retain unbuffered drawing.
- `app_view` owns the exact process/slot/generation tuple, copied name/document,
  start time, dirty classifications and output policy. Generations survive
  Terminal resets and WM slot reuse. Trusted I/O additionally requires READY or
  SLEEPING lifetime; EXITING, retained-DONE and reused owners cannot mutate a
  view. A deferred Stop immediately revokes UI eligibility, but preserves an
  already-active slice's explicit publication/output boundaries until return,
  including nonzero APP exit precedence. The Stop request itself never publishes.
  The hosted sink copies directly into the explicit Terminal's
  scrollback without changing selection or retaining text pointers.
- `app_view_poll_update` is the only desktop scheduler/completion consumer;
  retained DONE results are drained before another process slice. Historical
  `term_task*` and `term_canvas*` entry points remain compatibility wrappers.
- Terminal keeps its text, input/history/draft, cwd incarnation, scrolling and
  input-loss state. Synchronous input adapters and `program_key` are unchanged.
  Publication boundaries, the native byte-key queue and process resource cleanup
  remain unchanged.

The actual freestanding i386 layout is measured by `tests/app_view_layout.c`
and `test_app_view.py`, rather than inferred from a design budget:

| Item | Bytes |
|---|---:|
| Terminal text per slot | 27,432 |
| View metadata before canvas per slot | 72 |
| Working canvas plus publication metadata per slot | 64,032 |
| Complete view per slot | 64,104 |
| Eight text records plus eight views | 732,288 |
| Original Terminal container capacity | 786,432 |
| Unused container bytes | 54,144 |
| Published pixels, eight full frames | 512,000 |
| Original published arena capacity | 524,288 |

`AppStorage` is one typed aggregate at the original `APPS_BASE+0x300000`.
The published arena remains at `NATIVE_CANVAS_BASE`. Static assertions retain
both limits and the production link retains the kernel/stack and process-page
metadata guards. No extra physical arena, surface pages or GUI log is allocated.
The earlier 776,192-byte native-window design budget is not the measured layout
of this extraction.

New normal-operation tests cover direct view initialization before any Terminal,
explicit copied output and metadata, preserved command/history/input-loss state,
NONE-output print/completion, retained-DONE I/O refusal, deferred-close storage
retention, common poller compatibility and generation persistence. The real
process+view integration gate checks actual published pixels after deferred
Stop followed by present/yield/sleep/pending positive SYNC_WAIT/explicit exit,
immediate UI ineligibility,
APP exit precedence, nonpublishing timer/error/idle Stop and stale output refusal.
Existing publication, native binding, rectangle, input, launch, resource-lifetime
and renderer suites remain
the behavioral regressions. Exact final host/guest qualification is recorded
separately; this description alone is not guest qualification.
