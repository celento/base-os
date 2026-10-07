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
  not count. Multiple partial covers are not combined into a region.
- No canvas, or a zero-size draw rectangle, falls back conservatively. Partially
  visible background canvases still trigger full composition. No work is skipped
  inside the native task or frame-publication copy.
- Moving, resizing, minimizing or closing a cover follows normal scene
  invalidation. A full repaint reveals the latest complete published frame even
  if all of its previous canvas-only notifications were suppressed.

## Host verification

`test_native_render.py` runs under ASan/UBSan. It checks:

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

Native and foundation sanitizer runs use `ASAN_OPTIONS=detect_leaks=0` and
`UBSAN_OPTIONS=halt_on_error=1`; leak detection is therefore not covered.

These are host tests; they establish rendering equivalence and a narrower
redraw decision, not a measured responsiveness or storage-speed improvement.

See [complete-frame publication](NATIVE_CANVAS_PUBLICATION.md) for the unchanged
native frame visibility contract.
