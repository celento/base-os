# Hosted native pointer service, staged implementation

This candidate adds a kernel-owned endpoint for one existing native Terminal
canvas. It does not create windows, allocate surfaces, deliver keyboard events,
or implement a graceful close handshake. Existing BEX1 calls 0–27, byte-key
queues, explicit frame-publication rules and forced Stop/host-close remain intact.
Memory-info retains call 28 and feature bit 4; current-context BEX2 retains bit 5.
The coordinated UI registry uses gateway 29, feature bit 6, and the private
UI-target allocation domain 0x40000000. The general ABI query remains 96 bytes.

## Stage 1: pure geometry and endpoint core

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
numbers, geometry tokens and stream tokens do not wrap. Release and cleanup are
idempotent at the owner level; reopened targets have fresh identities.

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

## Wire contract

`sdk/baseos_abi.h` defines fixed four-byte word layouts and checked offsets:
64-byte UI query, 96-byte target info, 96-byte event. All reserved words are zero.
The local operations are QUERY=0, HOST_OPEN=1, INFO=2, READ=3, WAIT=4, RELEASE=5.
The negotiated event major is 1. POINTER is required; HOVER and WHEEL are optional.
No unsupported window/keyboard-event/surface capability is declared.

Intended gateway registers after EAX=29:

| Operation | EBX, ECX, EDX, ESI, EDI |
|---|---|
| QUERY | 0, requested major, output, capacity, 0 |
| HOST_OPEN | 1, requested major, output, capacity, subscriptions |
| INFO | 2, target, output, capacity, 0 |
| READ | 3, target, output, capacity, 0 |
| WAIT | 4, target, QUEUE optionally with LEGACY_KEY, milliseconds, 0 |
| RELEASE | 5, target, 0, 0, 0 |

The process boundary must check the full declared output span before allocation
or queue consumption. Query copies min(capacity, 64), with a 16-byte minimum;
OPEN/INFO/READ require at least 96 bytes and copy exactly 96. Errors/PENDING leave
all output untouched; successful copies preserve an unused output tail. WAIT
must not publish, retain a user pointer, or satisfy a separate sync wait.

## Executed stage-1 checks

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

## Stage 2: process boundary and SDK

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
present/yield/sleep/exit retain their pre-existing publication behavior.

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
