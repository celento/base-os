# Files: sorting and in-folder filtering

Files keeps each window's folder, sort order, name filter, selection and scroll
position independent. It supports the 360 × 200 minimum window and the 800 × 600
desktop mode without requiring a wide, four-column table.

## Sort controls

Click **Name**, **Type**, **Size** or **Modified** above the list. Clicking the
active control reverses its direction; an arrow shows the current direction.
Ctrl+1, Ctrl+2, Ctrl+3 and Ctrl+4 perform the same actions. A new window starts
with Name ascending. Selecting Modified initially puts newer timestamps first;
other newly selected keys initially ascend.

Folders always precede files/applications, including descending views. Name and
Type comparisons ignore ASCII case. Type uses a folder/application label or the
last file extension, so `.bwr`, `.bsh`, `.bex` and other associations group
naturally; extensionless and leading-dot-only names use `File`. Equal Type, Size
or Modified values use ascending names and then node IDs for predictable ties.
Name's case-insensitive ties use exact-case names and then IDs.

The name remains the main column. The right side shows the active key's useful
metadata: bytes for Size, UTC date/time for Modified, or type for Name/Type when
there is room. Narrow windows shorten the date to its date portion and hide the
optional type field. Folder/application sizes show their kind, not a misleading
recursive byte total. `Unknown` modification times are the existing zero
filesystem timestamps: they precede known times in ascending order and follow
known times in descending order. Properties still shows complete metadata.

## Filter this folder

Click **Filter** or press Ctrl+F, then type part of a name. Matching is an ASCII
case-insensitive substring, limited to 23 characters, the filesystem's maximum
name length. Filtering is local to the current folder; it does not search file
contents or subfolders and does not interpret wildcards.

- Ctrl+F or clicking the field selects its current text for replacement.
- Ctrl+A selects the filter text; typing or Backspace replaces/clears it.
- Backspace otherwise removes the final character. Input appends at the end.
- Enter or Tab leaves the field and keeps the filter visible. Arrow keys leave
  the field and select rows. Ctrl+C/X/V do not operate on files while typing in
  the field; leave the field first to use the file clipboard.
- **Clear** clears the text while keeping the field open.
- **Close** or Escape clears the filter and closes its row. Escape does not close
  the Files window while its filter row is open.
- The synthetic `..` row stays available. Opening another folder or going up
  clears the filter, while keeping that window's sort order.

The path strip shows the item count, or `N of M shown` when filtering. The parent
row and hidden system folders are not counted. Clipboard completion or unsaved
warnings can temporarily replace this strip, as before. Empty folders and
nonmatching filters get different messages.

## Selection and action safety

A selection is preserved through sorting and refresh only while both its node
ID and filesystem identity still name the same visible object in this folder.
Filtering it out, deleting it, moving it away or reusing its node slot clears
selection. Another row never becomes selected just because it takes the old
position. Opening, Copy/Cut, Duplicate and rename all reject stale rows.

A rename and an active Trash drag also retain their original object identity.
Replacing the object during either operation cannot redirect the operation to
its replacement. Paste targets the current folder, never a selected child
folder. A stale/reused folder context rejects the pending operation before
returning the view to the root.

Successful New File, New Folder, Paste, Duplicate and rename reveal/select the
exact completed result. If its new name is hidden by the current filter, the
filter text is cleared to reveal it. Existing collision-safe clipboard behavior,
copy naming, native `.bex` launching and disk-sync feedback are unchanged.

Sort/filter choices are temporary window state and are not saved across reboot.
There is no multiselection, recursive folder-size calculation or filesystem
format change.

## Verification

The view model in `src/file_view.c` has no rendering or mutation dependency.
Focused tests use valid volumes and ASan/UBSan:

```sh
ASAN_OPTIONS=detect_leaks=0 python3 -m unittest discover -s tests -p 'test_file_view*.py' -v
ASAN_OPTIONS=detect_leaks=0 python3 -m unittest discover -s tests -p test_file_clipboard.py -v
```

They cover ascending/descending keys, case/extension behavior, stable ties,
full 64/256-node volumes, local filters, remounts, nonmutating refresh, real
clipboard integration, and exact production selection/filter/rename/click/scroll
functions. Stale selection, rename, drag, and destination-folder tests use normal
file deletion/reuse, not guest-memory writes or fault injection. The Editor,
Spreadsheet recovery and native-launch host suites verify their shared desktop
contracts separately.

The normal production PS/2 flow can be checked with
`python3 tools/files_view_input_test.py build`. It uses disposable disks, keyboard
and pointer input, screenshots and independently decoded saved bytes, without
reading or writing guest memory. The [4 October checkpoint](session-2026-10-04/FILES_SORT_FILTER.md)
records the completed host/guest checks and real 800 × 600/minimum-window captures.
