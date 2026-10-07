# Paint save safety

Paint saves a new image under `/Pictures`. It does not track an owned saved
file, so it rejects every existing destination name, including ordinary files,
applications, directories and a picture saved earlier since boot.
Choose another name when the dialog says **Name exists. Choose another name.**
This intentionally replaces the former implicit overwrite behavior.

Opening or canceling **Save picture as** does not create `/Pictures`. If that
path is absent, successful Save creates the folder and image together. If an
ordinary file or application occupies `/Pictures`, the dialog reports
**Pictures is not a folder.** and preserves it.

## Failure and retry

Before changing the filesystem, Paint checks the snapshot lease, destination,
combined folder/file node requirement and projected file-data allowance. The
allowance includes both extra records when a missing Pictures folder and its
image take the IDE volume beyond 64 nodes. A full node table reports **Not enough
free file slots.**; insufficient bytes report **Not enough space for this
picture.**

A rejected create removes only a Pictures folder made by that save. A
returned write failure removes the new image and, if applicable, that new
folder. Existing sibling paths, nested directories, file bytes, identities,
content revisions and metadata remain intact. Rollback is safe because these
helpers do not dispatch another application or start a snapshot within the
compound create/write operation; filesystem background servicing is device-only.

**Disk saving. Retry Save shortly.** retains the canvas and the current dialog
name while an incremental snapshot owns filesystem RAM. After it finishes,
retrying Save rechecks the current name and capacity. Canvas pixels and undo
history are unaffected by failed saves. The dialog stays open on failure.

A successful Paint Save still stages its bytes in the filesystem's RAM view.
Normal autosync and System → Shutdown provide disk synchronization. There is
no a Paint durability ticket, owned-file binding, overwrite
confirmation, automatic retry or asynchronous dialog workflow.

## Unchanged image format

The output is exactly 16,008 bytes: the four ASCII bytes `BOS1`, little-endian
16-bit width 160 and height 100, then 16,000 palette-index pixels in row-major
order. Suggested filenames retain the historical `.pbm` suffix; the bytes are
BaseOS BOS1, not the Netpbm PBM format. Image Viewer continues to recognize the
file by its content.

## Reproducible verification

```sh
ASAN_OPTIONS=detect_leaks=0 python3 -m unittest discover -s tests -p test_paint_save.py -v
```

The host test extracts the current production `paint_init`,
`unique_untitled_pbm`, `paint_write_named` and `paint_save` functions, and runs
them against real `fs.c` with deterministic in-memory floppy/IDE devices. Its
UI seams capture dialog state and Files refreshes. The only write-error seam
returns an ordinary negative error before filesystem mutation. It does not use
fuzzing, invalid memory accesses or a substitute Paint save implementation.

Coverage includes:

- Open, cancel/reopen, default-name selection and a blocked Pictures path
- Existing ordinary-file, app and directory names, including nested children
- Full floppy/IDE node tables and the two-node missing-folder requirement
- Full IDE byte capacity, projected node-record overhead and exact-boundary retry
- Actual invalid-name creation rejection with rollback of a new Pictures folder
- Returned write failure with new/existing Pictures, rollback, sync/remount and retry
- Real incremental lease rejection across multiple steps, retained name and retry
- Canvas/history preservation, all existing node/data preservation after failure,
  Files refresh on success and exact 16,008-byte BOS1 comparison after remount
- Floppy and default IDE successful saves, including rejection of a second save
  to the same name

The QEMU checks run with:

```sh
python3 tools/ui_test.py build
python3 tools/paint_save_input_test.py build
```

The UI fixture `tests/ui_guest.c` reports `UI-FEATURES-PASS` and
`SESSION-RESTORE-PASS`; its directory-collision rejection and `regression.pbm`
save cover the same helpers. The production-kernel runner uses only normal PS/2
keyboard/mouse input, screenshots and System → Shutdown. It verifies the IDE
volume only after QEMU exits, then repeats the exact file checks after reboot.
No guest-memory/debugger observer or live-volume reader is used.

The runner opens and cancels Save and checks that `/Pictures` stays absent,
saves a blank image as `first.pbm`, changes the canvas with an ordinary pencil
click, and checks that a second save to `first.pbm` reports **Name exists.
Choose another name.** The same retained dialog then accepts `second.pbm`. Both
exact 16,008-byte BOS1 files and an unrelated guard file's complete
metadata/data are verified after shutdown and reboot. The default pencil click
produces a 3×3 mark centered at logical (60,48); the expected bytes include
those nine black pixels. Capacity/returned-write rollback scenarios remain
host-level tests.
