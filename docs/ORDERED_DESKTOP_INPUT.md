# Ordered desktop input foundation

This is an internal compatibility change. It adds no native event syscall,
subscription, SDK capability, application window or pointer ABI. Existing native
apps still read byte keys through their current Terminal-owned queue.

## Acquisition and routing

`input_ingress.c` is the sole keyboard/modifier/pointer decoder. The polled 8042
producer copies at most 32 controller bytes per ordinary poll. It never calls an
application, filesystem mutation, UI handler or renderer. `platform_poll()` can
therefore acquire input during rendering, storage and syscalls without changing
the desktop's currently dispatched coordinates or modifier state.

A fixed 256-record FIFO preserves controller-observed order. Pointer sample time
is completion of a valid three- or four-byte PS/2 packet, including when keyboard
bytes occur between its bytes. Each internal 64-byte record contains its serial,
routing epoch, PIT tick, coordinates, post-sample buttons, wheel and six physical
left/right Shift/Ctrl/Alt latches. Keyboard make-to-character translation remains
the existing US mapping; extended arrows/Enter remain compatible. Print Screen's
fake shifts and Pause's prefix sequence do not alter the physical latches.

The desktop consumes at most 64 records per turn, allowing normal storage,
cooperative tasks and rendering between batches. Existing compatibility handlers
receive each record once, in order. Pointer movement is applied to the current
gesture before that packet's release, so a final UP can carry its final position.
The old keys-first queue, accumulated clicks/wheel and end-of-turn release replay
are removed. Cursor movement is accumulated only for repaint damage; acquisition
cannot move the dispatched cursor. Double-clicks use sample ticks, including when
several clicks arrive during one long desktop operation.

## Cancellation, loss and scene lifetimes

Queue overflow discards the incomplete stream and latches one reset outside the
ring. Acquisition keeps maintaining the latest state; normal delivery resumes
only after the desktop consumes that reset. Reset cancels dragging, resizing,
file drops, selection and uncommitted Paint shapes without committing a release.
Held buttons are suppressed until individually released. Device loss additionally
keeps an unknown-button suppression latch across scene transitions until valid
pointer samples observe releases. Lost modifier bytes reset the physical latches
rather than retaining an unknowable pressed shortcut modifier.

The desktop records the routed window/overlay scene after each sample. Autonomous
changes to topology, window incarnation, geometry or blocking overlays advance a
pointer epoch before any queued pointer input is routed. Old pointer records are
consumed without hit-testing a replacement window; keyboard ordering remains.
An incomplete pointer packet's already-observed button flags also participate in
held-button suppression at a fence. A routed click's normal focus/geometry changes
are remembered as part of that stream, preserving its already queued DOWN/UP.
Keyboard-driven scene changes cancel existing gestures. Display-mode changes
always fence the coordinate space and clamp acquisition to the new bounds.

Opening a menu/modal cancels underlying gestures, but does not clear the modal's
own double-click history on every sample. Loss and autonomous lifetime fences
clear old click histories. Saver activation is checked within the sample loop;
release-only after activation does not wake it. A wake consumes the full already
acquired wake backlog, even across multiple 64-record turns, without click-through.

Synchronous BASIC/exec temporarily owns the same stream through a small Terminal
begin/end callback. INKEY drains the entire available bounded snapshot and retains its last-character
behavior. Unconsumed keyboard/modifier typeahead keeps its original ordering on
return, matching the legacy shell behavior. Pointer records are fenced instead
and cannot become deferred desktop clicks. The end callback first acquires the pending controller tail
with a separate finite 1024-byte budget while that ownership still applies. If
that budget fills, subsequent ordinary desktop turns keep consuming the tail in
32-byte units until an empty controller is observed, without replaying it.

## Evidence and limits

`tests/input_ingress_host.c` uses the production decoder and ring with ordinary
keyboard and mouse sequences: rapid edges, interleaved modifiers, packet completion,
left/right latches, clipping, wheel protocols, exact capacity, overflow reset and
normal repeated ring wrap. `tests/desktop_input_host.c` extracts the actual kernel
router/motion/release functions and uses observable app callbacks. It covers
64/65-record batching, sampled modifiers/timing, nested acquisition, final-UP
motion, window drag/resize, Paint completion/cancellation, picker double-clicks,
focus loss, same-slot replacement, incomplete-packet fencing, overflow suppression,
saver activation/wake backlog and synchronous ownership.

Those tests establish the internal state and callback contract; they do not
substitute for ordinary production guest UI checks. App handler/model/render
regressions remain in the existing host suite. Production guest checks must use
ordinary supported PS/2 actions and a disposable volume, and record exact build
hashes. No hosted native pointer endpoint is implemented or advertised here.
