# Calendar agenda storage and recovery

Calendar uses a bounded local agenda in the custom BaseOS kernel. It has no
Linux/POSIX dependency, accounts, network access, recurrence, reminders, alarms,
time-zone database or daylight-saving conversion. Appointment times are local
wall times entered by the user; the RTC is only a navigation convenience.

## Model bounds

- At most 128 appointments, each with a stable nonzero 32-bit ID.
- Gregorian dates from 1900-01-01 through 9999-12-31. Leap years use the full
  divisible-by-4, century and divisible-by-400 rule.
- An optional time from 00:00 through 23:59; an empty time field means all-day.
- Titles contain 1–95 printable ASCII bytes, with at least one non-space byte.
  Spaces and punctuation are preserved. No tabs, newlines, control bytes or
  non-ASCII text are accepted.
- IDs increase monotonically, survive reboot, and are never reused after delete.
  After ID 0xffffffff is allocated, new appointments are rejected rather than
  wrapping; existing appointments remain editable/deletable.
- Read order is date, all-day before timed appointments, time, then stable ID.
  Indexes can change after edits; UI selection must use IDs.
- Month navigation clamps the selected day (January 31 to February 28/29), and
  day/month navigation refuses movement beyond either supported endpoint.

One model-owned draft contains an optional existing appointment ID, date text
(up to 10 bytes), time text (up to 5 bytes), title (up to 95 bytes), active flag,
and focused field. Partial or invalid date/time/title text is deliberately
preserved. Commit strictly validates YYYY-MM-DD, HH:MM or empty time, and title;
invalid/full/exhausted-ID commits leave the complete draft untouched. A successful
commit updates/adds the appointment and clears the draft as one private-state
change before the next storage tick. Deleting the appointment currently being
edited is rejected until that draft is explicitly cancelled.

## Persistent file

The reserved path is `/prefs/calendar.v1`. Only a regular file with the supported
strict format is adopted. An unrelated, empty, truncated, malformed, newer-version
or checksum-invalid file is left untouched, and creates no shutdown blocker by
itself. New private changes remain available, but cannot overwrite that file.

The format is endian-independent little-endian binary, not a serialized C struct.
It is exactly `32 + appointment_count * 108 + 113` bytes: 145 bytes for an empty
agenda, and 13,969 bytes at full capacity, including a maximum-size recovery draft.
This is below the legacy floppy's 16,383-byte per-file limit. Padding is canonical
zero padding and is validated. No C struct padding reaches disk.

### Header, 32 bytes

| Offset | Type | Meaning |
| --- | --- | --- |
| 0 | 4 bytes | Magic `BCA1` |
| 4 | u16 | Version, 1 |
| 6 | u16 | Header bytes, 32 |
| 8 | u16 | Appointment count, 0–128 |
| 10 | u8 | Draft active, 0 or 1 |
| 11 | u8 | Draft field: 0 date, 1 time, 2 title |
| 12 | u32 | Next ID; 0 means exhausted |
| 16 | u32 | Draft edit ID; 0 means new appointment |
| 20 | u32 | Exact total file length |
| 24 | u32 | CRC-32 of entire file with bytes 24–27 treated as zero |
| 28 | u32 | Reserved, must be zero |

CRC-32 uses the reflected polynomial 0xedb88320, initial 0xffffffff and final
xor 0xffffffff. It covers header and payload. Filesystem snapshot verification
is still required for durability; this per-file checksum does not replace it.

### Appointment record, 108 bytes

| Offset | Type | Meaning |
| --- | --- | --- |
| 0 | u32 | Stable ID |
| 4 | u16 | Year |
| 6 | u8 | Month |
| 7 | u8 | Day |
| 8 | u16 | Minutes after midnight, or 0xffff for all-day |
| 10 | u8 | Title length, 1–95 |
| 11 | u8 | Reserved, zero |
| 12 | 96 bytes | NUL-terminated, zero-padded ASCII title |

Records must be in canonical sorted order, have unique valid IDs, and have IDs
below the next-ID value unless the ID space is exhausted. All calendar and title
bounds are checked before the decoded model replaces any live model data.

The final 113 bytes are zero-padded date[11], time[6], and title[96] draft fields.
An inactive draft requires all of these bytes, its edit ID, and field to be zero.
An active edit ID must identify an appointment in the same file. Active fields
need only bounded printable ASCII, so interrupted edits remain recoverable.

## Accepted edits, background saves and status

All CRUD and draft operations only change private model RAM and mark it queued.
They do not mutate the filesystem or perform disk I/O from the input path.
`cal_agenda_tick()` runs after the top-level storage pump and before autosync,
including when Calendar has no open window. Once the filesystem snapshot lease
releases, one tick stages the latest coalesced agenda and draft in filesystem RAM.
No filesystem mutation is attempted while `fs_sync_busy()` is true.

Status distinguishes queued private changes, filesystem-RAM changes waiting for
disk, verified saved state, occupied paths, full storage, changed source, returned
write errors, and failed/protected disk storage. A successful `fs_write()` is not
called saved. Only a matching destination identity/content plus a clean,
successfully synchronized filesystem establishes durable success.

Normal disk writes use existing filesystem autosync and its existing failure
backoff. Only queued work is retried automatically. Permanent/path/capacity/write
errors retain work without retrying allocation or mutation on every tick. Another
accepted edit, explicit Ctrl+S, or shutdown preparation can attempt staging again.
Explicit retry does not rewrite an already matching RAM copy just to retry disk
I/O. A busy explicit retry returns `FS_ERR_BUSY`; the model stays queued.

The loader uses a separate extra byte for `fs_read()`'s convenience NUL; the
maximum-size 13,969-byte binary file is never accidentally truncated by the
text-oriented read API. File buffers and decode scratch live outside the kernel
stack, and no borrowed filesystem data pointer survives a call.

## Conflict and allocation safety

Each staging operation resolves `/prefs` from the live root. A regular file/app
in place of `/prefs`, or a directory/app in place of `calendar.v1`, is protected.
The destination must still have the captured file ID, fresh filesystem identity,
length and exact source bytes. Same-ID replacement or external editing cannot
redirect or silently replace an agenda write. If the old file/directory moves
away, Calendar may recreate its reserved path while preserving the moved object.

Before creating either node, Calendar checks the total resulting node count,
file-size limit and volume byte allowance, including reduced IDE capacity when
adding metadata nodes beyond 64. A returned create/write failure rolls back only
nodes created by that attempt. Existing failed file writes retain their bytes.

If an already durable agenda without an active draft is changed externally,
Calendar reports that reopening will reload it, and does not invent unsaved work
or block shutdown. A new private edit based on that old view still refuses to
replace the changed file.

An active draft remains recovery work even if its old copy was durable. External
replacement therefore retains the draft and reports a conflict, rather than
suggesting a reopen that would discard it. Shutdown is blocked until the conflict
is resolved. Move the conflicting object away from the reserved path and press
Ctrl+S; the latest retained agenda and draft are then saved to the reserved path.
Calendar does not silently merge or overwrite another source.

## Lifecycle and kernel integration

- Call `cal_agenda_load()` when opening/reopening Calendar. Private pending work,
  RAM-only accepted edits and any active draft survive window close/reopen.
- Call `cal_agenda_tick()` after storage progress, before autosync. Its nonzero
  return means storage status changed and the UI should repaint.
- Call `cal_agenda_prepare_shutdown()` before staging the session and the final
  `fs_sync()`. It joins an older in-progress snapshot to release the lease, even
  if that snapshot fails, then stages the newest agenda and unfinished draft.
- A negative preparation result must cancel shutdown/reboot and leave Calendar
  available. Zero means ready in filesystem RAM only. The caller still checks
  the final `fs_sync()` after all shutdown writes before powering down/rebooting.
- Tick after a successful final/explicit sync to update the displayed status.
- Tick/preparation are no-ops before the model has been opened/loaded, so an
  unopened agenda file is never overwritten with a default empty model.

Explicit shutdown/reboot preserves unfinished date, time, title, field, and edit
ID without turning a partial draft into an accepted appointment. Power loss
before autosync can still lose queued/RAM-only work; status reports that risk.

## Verification

Run ordinary deterministic host coverage with the actual production filesystem:

```sh
ASAN_OPTIONS=detect_leaks=0 PYTHONPATH=tests python3 -m unittest test_calendar_agenda
```

The ASan/UBSan harness exercises valid and explicitly malformed documents,
Gregorian date edges and bounded navigation, stable sorted CRUD/ID exhaustion,
128 full-length titles with legacy-floppy remount, atomic draft acceptance,
invalid partial draft recovery, real snapshot-busy coalescing, missing/occupied
paths, same-ID reuse and changed source, active-draft external replacement,
capacity/metadata preflight, ordinary returned allocation/write failures and
rollback, no permanent-error retry spin, disk-failure backoff, read-only recovery,
and shutdown joining both successful and failed older snapshots. It uses no
intentional memory faults, fuzzing, debugger workaround or QEMU. Rendered UI and
guest-input/shutdown verification are separate checks.
