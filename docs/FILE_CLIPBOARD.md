# Files clipboard

`src/file_clipboard.c` provides the identity-safe file/folder clipboard used by
the desktop. It has no rendering dependency and uses the existing filesystem
copy, move and dual-snapshot synchronization paths.

## Operations and safety

- Copy or Cut selects one ordinary file or folder. Root, application nodes and
  folders containing application nodes are rejected. Rejected selection leaves
  any previous file clipboard intact.
- The selection stores both the node ID and `fs_identity`. Renaming or moving
  the source keeps the selection valid; deleting and reusing its node slot does
  not. Copy uses the source's current contents when Paste runs, rather than a
  snapshot of the contents at the time of Copy.
- Copy keeps the original name when it is unused in the destination. For a
  collision, including a same-folder copy, it adds a unique copy label before a
  normal file extension: `report copy.bwr`, `report copy 2.bwr`, and later names.
  The extension keeps its original case and the stem is shortened to fit the
  filesystem's 23-character limit. Folders and files without a normal extension
  retain the bounded `name copy` convention. Existing files are never
  overwritten. Recursive copies retain child names and data while allocating
  new identities.
- Cut makes no immediate filesystem change. Paste moves the existing object
  with its original identity. Unlike the general `fs_move` auto-renaming path,
  this clipboard rejects a name collision before calling `fs_move`. The source
  stays selected for Cut after a rejected move.
- Both operations reject a folder's own subtree as a destination. `fs_copy`
  and `fs_move` retain responsibility for their atomic capacity and path-depth
  checks. A partial recursive copy rolls back its allocations and dirty flag.
- Pasting a Cut into its current parent consumes the selection as a harmless
  no-op. It does not rename, write a disk snapshot or claim any pending changes
  were saved.
- A successful RAM move consumes Cut immediately. If disk synchronization
  fails, Paste still reports a completed RAM operation. It does not leave the
  old Cut available to move the source a second time.

## RAM completion versus disk completion

`file_clipboard_paste(directory, &result_node)` returns one of four outcomes:

| Result | Meaning |
| --- | --- |
| `FILE_CLIPBOARD_ERROR` | No new file or move retained; check the status message. |
| `FILE_CLIPBOARD_NOOP` | Same-folder Cut was consumed without changing files. |
| `FILE_CLIPBOARD_SYNCED` | The operation, or a prior completed paste's retry, has a confirmed disk sync. |
| `FILE_CLIPBOARD_RAM_ONLY` | The filesystem operation completed in RAM, but synchronization failed. |

The output node is initialized to `-1`, then set only when a completed result
still exists with the recorded identity in the requested directory. The UI may
select or reveal this node for either saved or RAM-only results.

After a Copy returns `RAM_ONLY`, the next Paste only retries synchronization. It
does not make another copy, including if filesystem autosync completed before
that retry. After the retry succeeds, a subsequent Paste can make another copy.
An explicit new Copy/Cut or a new text clipboard write ends the retry guard.
Deleting or reusing the completed copy's node cannot cause a retry to select its
replacement, nor can deleting the original cause the retry to copy another file.

For a consumed Cut, the normal filesystem autosync or an explicit filesystem
save can persist the outstanding RAM change. `file_clipboard_pending_sync()`
describes this module's last unconfirmed completion; a new selection or clear
resets it. The filesystem's `fs_needs_sync()` remains the authoritative global
dirty state.

## Desktop integration contract

1. Compile and link `file_clipboard.c` with the kernel.
2. After a successful `file_clipboard_set`, make the shared clipboard's type
   file-copy or file-cut and invalidate any styled text clipboard generation.
   A rejected selection must not invalidate a previous clipboard.
3. Whenever any app writes new text into the shared clipboard, call
   `file_clipboard_clear()`. This cancels the pending file selection or copy
   retry guard; it never undoes completed filesystem changes.
4. Use `file_clipboard_can_paste(current_folder)` to enable Files Paste. This
   query checks source identity, ordinary-node restrictions, destination type,
   ancestry and move-name collisions without changing status. Capacity, path
   shape and disk I/O may still fail when the operation runs.
5. Use `file_clipboard_paste` for the action and refresh Files windows after a
   completed RAM mutation. Treat `RAM_ONLY` as an unsaved warning, never as a
   claim that the operation was rejected or that nothing changed. Consume the
   shared file-cut type whenever `file_clipboard_mode()` becomes `NONE`.
6. Display `file_clipboard_status()` for bounded human-readable feedback. The
   string is always NUL-terminated and shorter than 128 bytes. Name, source,
   identity and mode getters are available for menu/selection information.
7. Use `file_copy_named(source, destination)` for another copy route, such as
   File > Duplicate, so it preserves the same file associations. This public
   helper returns a new node or `-1` and only changes the filesystem in RAM.
   It does not synchronize or change any clipboard selection, status or pending
   retry state. The caller owns synchronization and its user-facing feedback.

This clipboard is RAM-only and is not restored across reboot. It does not
implement multi-selection, links, clipboard file content snapshots or file
operation undo.

### Copy naming and atomic completion

`file_copy_named` chooses a free final name before it allocates anything. A
normal extension is the last non-leading dot followed by one or more ASCII
letters or digits. Multiple-dot names preserve the final extension, such as
`read.me copy.html`; a lone leading dot or trailing dot is not an extension.
The complete extension and at least one stem character must fit alongside the
copy label. An unusually long extension that cannot fit is rejected rather than
truncated. An unused original name is always preserved.

The helper lets the existing atomic `fs_copy` create its generated name, then
renames that new copy to the chosen final name in RAM. Paste calls `fs_sync`
only after naming is complete. The global `fs_copy` implementation is unchanged,
and no intermediate name is synchronized. This rename is guaranteed under the
current valid-volume contracts: the selected name is valid and free, the
successful copy already validated subtree depth, all legal-depth paths fit in
`FS_PATH_LEN`, and filesystem background servicing cannot mutate the tree
during the operation. A compile-time assertion records the path-size invariant.
If a future contract change nevertheless makes the rename fail, the helper
deletes only its new copy before returning an error. That defensive fallback
can conservatively leave the filesystem dirty, but retains no extra copy and
never deletes or overwrites an original file.

## Verification

Run the focused normal host suite:

```sh
ASAN_OPTIONS=detect_leaks=0 python3 -m unittest discover -s tests -p test_file_clipboard.py -v
```

The suite compiles the module separately with AddressSanitizer,
UndefinedBehaviorSanitizer and warnings as errors. It uses valid floppy and IDE
volumes, binary/empty files, live renames and edits, deletion/reuse, repeated
long collision names, `.bwr`/`.mp3`/`.html` association-preserving names,
case-preserved extensions, standalone RAM-helper state isolation, recursive
copies, identity-preserving moves, same-folder
no-op, app/subtree/collision rejection, full 64/256-node limits, partial copy
rollback, full data allowance, deepest supported paths and successful remounts.
A temporarily unavailable mock storage device checks the ordinary RAM-only
result and retry flow. It does not use malformed-volume fuzzing or CPU/memory
fault probes. Desktop menu/keyboard interaction is verified separately by the
integrated guest tests.

If `/tmp` has no free space, point `TMPDIR` at a writable scratch directory with
room before invoking the suite; both Python and the C compiler honor it.


## Verified desktop controls

Files supports Ctrl+X / Ctrl+C / Ctrl+V and the Edit menu. A pending Cut is marked
in the row; bounded operation feedback temporarily replaces the path strip.
Completed mutations refresh every open Files window and select the resulting
node. File and text clipboard formats are exclusive: a file cannot paste stale
text or delete a Writer selection, and new text clears pending file selections.

`tools/file_clipboard_input_test.py` verifies actual production keyboard/menu
input, safe repeated copies, Cut collision rejection, identity-preserving moves,
same-folder no-op, recursive copy and subtree rejection, an exact 65,792-byte
binary, source deletion/reuse, Writer/Editor clipboard separation and reboot
persistence. Tests use new disposable images and inspect bounded compiled data
layouts without writing guest memory.
