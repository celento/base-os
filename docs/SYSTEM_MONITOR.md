# System Monitor

Monitor separates information into three tabs:

- **System** shows processor identification, uptime, detected RAM, display mode,
  actual compositor redraws, and filesystem node/payload usage.
- **Windows** lists open applications and minimized state, with the existing
  Close control. Closing a Terminal also ends its native task.
- **Tasks** lists live terminal-owned native programs. Each row shows the copied
  executable basename, Running or Sleeping, owning Terminal slot (1–8), and
  elapsed wall-clock lifetime. Show terminal restores/focuses that terminal;
  Stop task ends only its native task and leaves the terminal open for commands.

All eight rows fit at the standard 520 × 439 client size (the default 440-pixel
content allocation loses one pixel to the title boundary). The same calculated
geometry is used for drawing and hit testing after resizing or maximizing.
Text and drawing stay within the supplied client bounds. If a caller supplies a
smaller-than-supported client, only complete rows receive controls and a footer
asks for more room. The monitor does not claim to measure whole-system or
per-task CPU utilization. Task lifetime includes sleep and time waiting for a
slice; it is not CPU time.

Native task state and elapsed lifetime update on the desktop's periodic redraw.
Executable names are copied when Terminal accepts `start FILE`, so renaming,
deleting or reusing the source filesystem node cannot change a running task's
label. A rejected start leaves the existing task's metadata unchanged. Task
metadata disappears on completion, Stop, terminal close, or terminal reset. Live
task execution state is not persisted across reboot.

## Terminal query API

`int term_task_info(int owner, TermTaskInfo *out)` accepts a zero-based terminal
slot and copies a snapshot without calling `term_select` or changing terminal
input/output/selection. It returns 1 only for `PROCESS_TASK_READY` or
`PROCESS_TASK_SLEEPING`; otherwise it returns 0 and clears the supplied output.

`TermTaskInfo` contains:

- `name[TERM_TASK_NAME_LEN]`: copied executable basename, including a NUL
- `owner`: zero-based terminal/window slot
- `state`: the live process state from `process_status`
- `started_ticks`: PIT tick counter at the accepted start
- `elapsed_sec`: unsigned PIT tick difference divided by `TIMER_HZ`
- `instance`: the full nonzero opaque process handle, distinct even for immediate
  same-slot, same-tick restarts

The unsigned tick subtraction handles a PIT rollover for lifetimes shorter than
one complete 32-bit tick cycle. Low-level process creation is independent of
Terminal and does not appear in this display snapshot without a Terminal
attachment. Ordinary user-started tasks always use the tracked Terminal
lifecycle and copied filename/start metadata.

## Desktop integration contract

`SysInfo` adds `task_n` and `tasks[SYSMON_MAX_TASKS]`. Build each snapshot from
currently open Terminal slots, preserving the slots as owners:

```c
si->task_n = 0;
for (int i = 0; i < MAX_WIN && si->task_n < SYSMON_MAX_TASKS; i++) {
    if (wins[i].open && wins[i].kind == WK_TERM &&
        term_task_info(i, &si->tasks[si->task_n]))
        si->task_n++;
}
si->uptime_sec = timer_ticks() / TIMER_HZ;
si->frames = redraw_count;
```

`frame_count` in the desktop is the PIT tick counter, not rendered frames. Do not
populate `SysInfo.frames` from it. `redraw_count` counts actual full desktop
compositor redraws; the label does not promise display refresh rate or total GPU
work. The existing `redraws` member remains for source compatibility.

Call `sysmon_draw(bx, by, bw, bh, &si)` with the client bounds. For clicks use the
same bounds, including height:

```c
SysmonAction action = sysmon_click(bx, by, bw, bh, mx, my, &si);
switch (action.kind) {
case SYSMON_ACTION_REDRAW:
    dirty = 1;
    break;
case SYSMON_ACTION_CLOSE_WINDOW:
    win_close(action.owner);
    break;
case SYSMON_ACTION_SHOW_TERMINAL:
case SYSMON_ACTION_STOP_TASK: {
    TermTaskInfo live;
    int owner = action.owner;
    if (owner >= 0 && owner < MAX_WIN && wins[owner].open &&
        wins[owner].kind == WK_TERM && term_task_info(owner, &live) &&
        live.instance == action.task_instance) {
        if (action.kind == SYSMON_ACTION_SHOW_TERMINAL)
            win_focus(owner); /* also clears minimized state */
        else {
            term_task_stop(owner);
            dirty = 1;
        }
    }
    break;
}
}
```

`SYSMON_ACTION_NONE` requires no work. The monitor only returns actions; it never
closes windows, selects terminals or stops tasks itself. It remembers the task
identities in its last rendered rows and matches them against the current
snapshot at click time. If a task exits, other rows shifting in the fresh query
cannot turn a pending Stop click into a stop of another task. A new instance in
a reused slot is not accepted as the old target. The desktop recheck above
preserves that contract through routing.

Tab switching invalidates the last rendered row map until the next draw. The
optional `sysmon_reset()` returns a newly opened monitor to System;
`sysmon_tab()` exposes the current tab for normal integration tests. Monitor is
a single-instance app, matching the desktop's existing window policy.

Storage bars avoid `width * used_bytes`: i386 `long` is 32 bits and an ordinary
488-pixel, nearly-8-MiB bar previously overflowed it. The bounded pixel-width
accumulator computes the exact floor of the fraction without multiplication or
64-bit division helpers. It handles empty, partially used and full volumes.

## Deterministic host verification

```sh
ASAN_OPTIONS=detect_leaks=0 python3 -m unittest discover -s tests -p 'test_sysmon.py' -v
```

The renderer test uses the real software compositor, checks every drawing call
and every pixel outside the client, verifies all eight rows and both task
controls at minimum and expanded sizes, switches tabs, exercises empty lists,
and checks stale row/restarted-slot protection. Full action captions must fit.
The storage checks compare ordinary empty/partial/full 64-node, floppy, and
8-MiB-volume fractions against an independent 64-bit host reference.

The Terminal test uses the real command parser and filesystem with a normal
process-lifecycle fixture. It checks two independent task owners, copied names
after source rename/deletion, Running/Sleeping and lifetime, a busy-start
rejection, read-only query behavior, stopping one owner without affecting the
other or closing Terminal, completion, close, reset, immediate restart, and
unsigned timer rollover. These are deterministic functional tests, without
fuzzing or intentional memory-fault/privilege probes.

## Production desktop verification

```sh
python3 tools/sysmon_input_test.py build
```

This script boots the normal production kernel with newly created disposable
floppy/data images. It uses real launcher, keyboard and PS/2 mouse controls; ELF
symbols and QMP memory reads inspect outcomes without modifying guest memory.
It verifies two actual counter programs and their names, state and lifetime;
Show terminal restoring a minimized owner; maximizing and then stopping one
task while both terminals remain open and the other task continues to save;
eight visible Windows rows at the standard minimum size; Windows Close ending
its terminal's task; the empty Tasks view; and shell input in the stopped owner.
It saves screenshots in its printed temporary directory. The struct-size checks
fail early if the inspected normal-kernel layouts change.

The first integrated production run passed on `c790ffb` using this script. The
three-tab renderer and Terminal tests above also passed with ASan/UBSan. This is
normal functional coverage, not a claim of exhaustive process or security
verification. `tools/task_input_test.py` separately covers the broader native
task launcher, keyboard input, Ctrl+C, close, restart and native-exit lifecycle.
