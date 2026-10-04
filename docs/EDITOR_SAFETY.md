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
