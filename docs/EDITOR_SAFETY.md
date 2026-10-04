# Closing Editor documents safely

Editor checks for unsaved work when you use its title-bar close button,
File → Close, Ctrl+W, or System Monitor → Windows → Close.

- **Save** writes the entire document and verifies the filesystem snapshot on disk
  before closing. An unnamed document, or one whose original file was deleted and
  whose node ID was reused, opens the existing Save As dialog instead.
- **Discard** explicitly closes without saving the current document.
- **Cancel** or Escape keeps the document, caret, selection, and undo history.
  Cancel is initially selected. Tab and Shift+Tab cycle through Save, Discard,
  and Cancel; Enter activates the selected button. Each button supports the mouse.

An untouched new empty Editor closes immediately. An edited document still
requires a decision after deleting all its text, including a loaded file reduced
to zero bytes. Existing conservative undo/recovery dirty markers remain in use.

Canceling Save As cancels the close request. A rejected filename, full volume,
read-only recovery disk, or failed verified disk commit leaves the document open;
failed commits retain its unsaved marker. Save As reports a failed save rather
than silently dismissing. Ordinary Editor saves also clear the unsaved marker
only after successful disk synchronization. A failed synchronization can leave a
pending in-memory filesystem write; the live Editor buffer remains available.

## Scope and implementation

The close dialog owns a specific Editor window incarnation. It cannot close a
later window that happens to reuse the same slot. File writes retain the existing
filesystem-identity check; a reused numeric file ID is never treated as the old
file. Save As uses its explicitly entered destination name and the existing
current-folder behavior.

The modal blocks unrelated keyboard, mouse, wheel, menu, taskbar, and right-click
actions. Clicking outside it does nothing. Other windows, native tasks, network
work, and media continue in the cooperative desktop loop. The dialog participates
in the compositor's drag/video fast-path guards, and automatic screen saving and
session snapshots wait until it is dismissed. Save As has the same safeguards.

The dialog is 460 × 184 pixels and fits the minimum 800 × 600 desktop. State is a
small set of scalar owner/focus/error fields; there is no new document arena.
Internal `win_close` remains available for deliberate teardown, while user-facing
close routes use `win_request_close`. Paint is unchanged because its shared canvas
already survives closing its window. Shutdown retains the separate existing
session-draft and filesystem-flush workflow.

## Reproduction

```sh
make
ASAN_OPTIONS=detect_leaks=0 make test
python3 tools/editor_close_test.py build
python3 tools/editor_binding_test.py build
python3 tools/editor_input_test.py build
python3 tools/editor_test.py build
python3 tools/ui_test.py build
```

The close test boots the normal production kernel. All actions use real PS/2
keyboard/mouse events through QMP. Read-only ELF/DWARF-guided observations verify
window ownership, complete document bytes, selection, save state, and media
progress. No test kernel, guest-memory modification, fault probe, or main saved
image is used. Screenshots and disks remain in the printed temporary directories.

Coverage includes new and changed named documents, an emptied loaded file,
Save/Discard/Cancel, cancelled and rejected Save As, a deleted file's reused node
ID, repeated close, another unaffected Editor, System Monitor's close button,
modal pixels during video/audio/native-task activity, exact committed file bytes
after a real reboot, and failed disk saves in the supported unknown-disk read-only
recovery mode. The unknown disk is verified unchanged after the run.

Tests that deliberately close a dirty Editor through the user interface must
choose Save or Discard, or Cancel to continue editing. Internal fixture teardown
through `win_close` is unchanged. A pending Save As now returns incomplete from
`edit_save`; success means a fully synchronized file.

### Save As folder identity

Editor and Writer retain the identity of their containing folder, not only a
reusable filesystem slot. If that folder is removed and its slot reused, Save As
falls back to the root rather than silently writing into the replacement folder.
A still-existing folder remains selected. Editor restores its saved file's parent
folder after reboot. Editor Save As rejects unrelated existing names; it permits
retrying its own identity-matching file after a failed synchronization.

`tools/save_folder_input_test.py` exercises these paths through the normal desktop,
checks unchanged replacement folders and an existing-name collision, and reboots
the disposable disk to verify the restored folder context.

## Source versions and concurrent edits

![A stale second Editor preserves its draft and asks for a new name](../screenshots/editor-source-conflict.png)

Each of the eight Editors retains its own source baseline: the original byte
count, FNV-1a and CRC32. Save checks both the file's runtime identity and that
baseline before changing any bytes. Opening the same file twice, or replacing
its bytes from Terminal/a native app, therefore cannot let a stale Editor
silently overwrite the newer version. A conflict opens Save As with
**“Source changed. Save with a new name.”** The complete draft stays open and
unsaved; retrying the original name is also rejected. Saving a new name preserves
both versions. Renaming the same file is safe, and rewriting identical bytes does
not cause a conflict. Fingerprints detect accidental changes, not adversarial
replacement or cryptographic identity.

The baseline uses 16 additional bytes per window outside `Document` and its undo
snapshots. Undo/redo can change text without rolling back the source baseline.
A successful RAM write immediately updates that baseline, even when the disk
commit fails. Both direct Save and an owned-target Save As can then retry safely;
neither clears the unsaved marker until synchronization succeeds. A failed new
file write removes its empty target, while an owned write awaiting synchronization
is retained, including an intentionally empty document. Save As preflights the
volume's per-file limit before allocating a target.

### Recovery bindings

The existing version-1 `prefs/session` and `prefs/draft0.txt` through
`prefs/draft7.txt` remain readable. A separate version-1, 248-byte
`prefs/editor-bindings` record stores eight cached source baselines. Its header
also fingerprints the complete session record; each slot fingerprints its draft.
This pairing prevents mixed recovery generations from authorizing a save to an
unrelated prior source. No serialized runtime identity is trusted across reboot.

A recovered draft only binds to its resolved source when the sidecar, session,
draft and current source all match. Missing legacy metadata, unsupported metadata,
changed source bytes or mismatched draft/session bytes recover an unbound, dirty
document that requires a new Save As name. The source is untouched. A matching
recovery retains the original source baseline, rather than adopting the unsaved
draft as the source version. Folder-identity safeguards continue to apply.

Session saving preflights every required node, including a missing `prefs`
directory and the binding sidecar, the complete draft sizes, and the byte budget
at the projected node count. Existing shrinking drafts are written before any
new draft node, including an empty one; metadata and the session record follow.
Per-file/total/node-capacity rejection changes no earlier drafts. Writes must
report the complete requested length. Filesystem disk snapshots still provide
the existing verified dual-snapshot commit; draft metadata pairing also makes
an incomplete in-memory session update conservative on recovery.

### Focused verification

`tests/test_editor_binding.py` compiles the exact production Editor/session
functions with the real filesystem and history modules under ASan/UBSan. It
covers concurrent copies, same-byte writes, rename, undo baselines, reused IDs,
failed-create rollback, safe retries in supported read-only recovery, eight
recovered windows including a 65,535-byte draft, and node/byte/per-file preflight.
It also exercises shrinking an existing draft before creating a zero-byte draft
on a full volume with more than 64 nodes.

`tools/editor_binding_test.py` uses the production kernel and real PS/2 events.
It checks two Editors sharing a file, an ordinary native app's same-ID/same-size
replacement through Terminal, rejected original-name retries, separate saved
copies, undo after save, paired recovery after real reboots, changed sources,
missing legacy metadata, changed drafts, and read-only failed-sync retries.
Saved source bytes are independently read from disposable disk snapshots, and
the unrecognized read-only volume must remain byte-for-byte unchanged. Neither
test uses guest-memory modification, an intentional memory-fault probe or fuzzing.
