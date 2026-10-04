# Todo persistence during snapshots

Todo keeps its existing `/prefs/todo` line format, twelve-item limit and
23-character task text. A leading space means incomplete; `x` means complete.
The reader still accepts an uppercase `X`. No disk format changes are involved.
Unrelated text, lines beyond 23 characters, more than twelve nonempty items and
files beyond the complete bounded format are rejected without binding or
overwriting them. A final line without a newline and empty separator lines remain
compatible. Rejection alone does not create unsaved work or prevent shutdown.

## Accepted edits and background saves

Adding a task with Enter or toggling its completion accepts the change into the
private Todo list. It attempts a filesystem RAM write immediately when storage
is available. During an incremental snapshot's mutation lease, multiple edits
coalesce in that private list. `todo_tick()` writes the latest list once the
lease ends, without requiring another user edit. It never attempts filesystem
mutations while the lease is held and does not initiate disk I/O.

A successful RAM write is not durable success. Status distinguishes queued
changes, changes in RAM awaiting a disk save, verified saved tasks, path or
capacity/write failures, and disk failures/read-only recovery. Existing
filesystem autosync owns normal disk persistence and its retry backoff. Todo
does not add a retry loop around disk failures.

Closing and reopening Todo preserves accepted changes that have not reached
filesystem RAM, changes still waiting for durability, and unsubmitted field
text. Pressing Enter clears the field only after its text joins the retained
private list. A full list leaves the unsubmitted field intact. Unsubmitted field
text is a session-only draft; shutdown does not implicitly turn it into a task.

## Failure safety and retry

Path resolution always starts at the live filesystem root. A file/application
occupying `/prefs`, or a directory/application occupying `/prefs/todo`, is never
overwritten. Existing regular Todo files must still have their captured identity
and exact source bytes. Deleted/reused node IDs or another app's edits cannot
silently redirect a pending save. If the old file or directory moved away, Todo
may safely recreate its fixed path without modifying the moved file. An external
change after Todo's list was already durable produces a reload notice rather
than new unsaved work: reopening can read the changed file, and shutdown is not
blocked. A new accepted edit based on that old view still requires an unchanged
source and cannot overwrite the external change.

The model preflights both new nodes and file bytes, including the lower IDE
payload allowance when adding nodes beyond 64, before creating a directory or
file. A rejected compound operation rolls back only nodes created by that call.
An existing file's failed write leaves its old bytes intact. Every failure keeps
the latest accepted private list.

Only a busy/queued write retries automatically. Path, source-conflict, full
volume and other write failures remain visible without mutation retry-spinning.
Another accepted edit, an explicit retry, or shutdown preparation can retry.
For a path/source conflict, move the unrelated/changed file away from the fixed
Todo path before retrying; retry never overwrites the conflicting file.

`todo_retry_save()` is the explicit Ctrl+S operation. It writes pending task state
and calls the blocking `fs_sync()` compatibility API. Return zero means verified
durable success, -1 means failure, and `FS_ERR_BUSY` means a snapshot is still
active. In the busy case, a pending list remains queued for `todo_tick()`.
Explicit retry does not rewrite an already matching filesystem RAM copy merely
to retry its disk save.

## Kernel integration contract

- Call `todo_tick()` after top-level storage progress and before the next
  autosync admission, even when no Todo window is open. A nonzero return means
  the displayed status changed and needs repainting.
- Show `todo_status()` on its own footer line and route Todo's Ctrl+S to
  `todo_retry_save()`. Increasing the footer should also increase the default
  window height to keep all twelve rows visible.
- Before staging session state and the shutdown's final `fs_sync()`, call
  `todo_prepare_shutdown()`. Abort shutdown if it returns a negative result and
  show Todo's retained state/status. Zero means the latest accepted list is in
  filesystem RAM, not that it is durable.
- Preparation joins an existing snapshot to release its lease. Even if that
  older snapshot fails, it stages the latest accepted list once the lease ends.
  The caller's final `fs_sync()` must check durability after all shutdown writes;
  the result of the older joined snapshot cannot certify newer Todo changes.
- After a successful explicit/final sync, ticking Todo updates its saved status.

## Verification

Run the ordinary host workflow suite with:

```sh
ASAN_OPTIONS=detect_leaks=0 PYTHONPATH=tests python3 -m unittest test_todo_pending
```

The suite compiles the production Todo and filesystem models with ASan/UBSan.
It verifies real lease coalescing, no mutations during busy polling, retained
list/draft across reopen, missing-path deferral, post-save remount, stale node
reuse, app/directory collisions, same-ID replacement and same-file content
conflicts, byte/node/metadata-capacity preflight, ordinary returned write/create
failures and rollback, no permanent-error retry spin, shutdown during success
and failed snapshots, existing autosync backoff, explicit disk-only retry,
read-only recovery, the legacy floppy path, and unchanged text/list limits.
These host tests do not claim a QEMU responsiveness or rendered-UI result.
