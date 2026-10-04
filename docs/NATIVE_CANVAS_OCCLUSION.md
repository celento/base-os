# Conservative native canvas occlusion

`TERM_TASK_CANVAS` means a newly published complete frame with unchanged layout.
The desktop can now avoid repainting that update when its entire scaled canvas
rectangle lies inside one higher window's opaque client interior. This includes
equal-size maximized Terminal windows, whose coincident borders prevented the
older whole-window containment test from suppressing a background canvas update.

The damage rectangle uses the same `term_canvas_geometry` function and origin as
the renderer. `term_canvas_size(slot, ...)` reads the explicit owner's visible
frame dimensions without changing Terminal selection. Native working pixels and
working resize metadata remain inaccessible through this query until publication.
The query is internal to the kernel, not a syscall or SDK ABI addition.

## Conservative boundaries

- Only the exact `TERM_TASK_CANVAS` flag is eligible for the new path. Text,
  layout, and any combination of flags retain the old whole-window rules.
  Lifecycle changes still force a full repaint, including minimized tasks,
  because taskbar/window titles may change.
- The existing whole-window check is unchanged. The new check requires a stable
  scene: no dirty flag, dialogs, menus, launcher, display confirmation, screen
  saver, drag/resize/cache state, file rename/drag, paint/editor interaction, or
  held mouse button. The same predicate still gates existing front-client partial
  repaint, without changing its requirements.
- Containment uses an eight-pixel horizontal/bottom inset and starts below the
  titlebar. These intentionally conservative opaque client bounds exclude rounded
  corner masks and border pixels. Closed, minimized and lower/equal-z covers do
  not qualify. Multiple partial covers are not combined into a region.
- No canvas, or a zero-size draw rectangle, falls back conservatively. Partially
  visible background canvases still trigger full composition. No work is skipped
  inside the native task or frame-publication copy.
- Moving, resizing, minimizing or closing a cover follows normal scene
  invalidation. A full repaint reveals the latest complete published frame even
  if all of its previous canvas-only notifications were suppressed.

## Host verification

On 2026-10-04, `test_native_render.py` passed under ASan/UBSan. It checks:

- Pixel-for-pixel equivalence to full composition using the production Terminal
  renderer, window chrome, z-order traversal, rounded corner masks, cursor
  restore/save path and 16/24/32-bit presenter. Only application dispatch and
  ordinary per-owner Terminal fixture data are substituted in this renderer test.
- Independent canvases under identical maximized windows at 800x600, 1024x768,
  1280x720 and 1280x800, with both 160x100 and 320x200 source frames.
- Full-window containment, exact opaque-area boundaries and one-pixel misses,
  genuinely exposed canvas strips, rounded corner exposure, minimized/lower
  covers and owners, multiple partial covers, and moving covers to reveal the
  most recent suppressed frame.
- Every existing stable-scene guard, all canvas/text/layout/lifecycle flag
  combinations, theme invalidation, and drag-cache invalidation.
- A separate fixture against the real Terminal checks all eight explicit owner
  queries, preserved unrelated selection/input, unpublished working geometry,
  published resize, Stop, close/reuse, clear, and synchronous `exec` compatibility.

The complete `test_native*.py` suite passed 20 tests, including unchanged frame
publication and its process boundaries, old executable fixtures, SDK behavior,
native platform dispatch/lifetimes, owned sync and large-profile reload checks.
Supporting checks passed: `test_kernel_layout.py` (5), `test_terminal_move.py`
(1), `test_video_draw.py` (1), and `test_foundation.py` (14). Native and foundation
sanitizer runs use `ASAN_OPTIONS=detect_leaks=0` and
`UBSAN_OPTIONS=halt_on_error=1`. The first foundation invocation reached an
unsupported LeakSanitizer/ptrace environment error; the ordinary ASan/UBSan rerun
passed all 14 checks. Leak detection is not claimed.
Production `make -j4` succeeded: 500,524 initialized kernel bytes and 338,341
packed bytes, from the `8c96dfc` base plus this change.

No QEMU run or timing comparison is claimed here. This establishes host rendering
equivalence and a narrower redraw decision, not a measured responsiveness or
storage-speed improvement. Identical application binaries and workloads must be
compared in the serialized production desktop runner before making such a claim.

See [complete-frame publication](NATIVE_CANVAS_PUBLICATION.md) for the unchanged
native frame visibility contract and prior production verification.
