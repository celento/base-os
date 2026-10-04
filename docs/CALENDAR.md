# Calendar appointments

Calendar keeps the small month view and adds a local day agenda. Click a date
or use Left/Right to select a day; dots mark dates with appointments, a circle
marks today, and an outline marks the selected date. PgUp/PgDn or the month
arrows move by month. Today or Home returns to the RTC date.

The selected day's appointments are sorted with all-day entries first, then
24-hour time, then stable insertion ID. Click a row or use Up/Down to select an
appointment. The agenda arrows page through six rows at a time. The complete 620 x 474 client fits the
800 x 600 desktop with its menu bar, taskbar and window title.

- New, N, Ctrl+N, or File > New starts an entry for the selected day.
- Enter or Edit edits the selected appointment. The original stays unchanged
  until the entry is accepted.
- Date uses `YYYY-MM-DD`, from 1900 through 9999. Time is optional `HH:MM` in
  24-hour notation; blank time or All-day makes an all-day appointment.
- Titles accept up to 95 printable ASCII characters and must contain more than
  spaces. The agenda holds at most 128 appointments.
- Click a field or use Tab/Shift+Tab. Left/Right, Home/End, Backspace and Delete
  edit the current field. Ctrl+A selects the whole field for replacement.
- Save or Enter accepts the entry. Validation or a full agenda keeps every
  field available to correct or retry.
- Cancel asks before discarding the unfinished entry. Delete asks before
  removing the selected appointment. Enter confirms; Escape or Keep cancels the
  confirmation. Repeated Delete does not confirm a deletion.
- Escape, the close button and Ctrl+W close the window while retaining the
  unfinished entry. Opening a second new/edit request cannot overwrite it.
  Click a date to browse without changing the draft; Tab returns to its fields.
- Ctrl+S, File > Save and Retry synchronize the current agenda and unfinished
  entry. They do not submit an unfinished entry as an appointment.

## Durability and recovery

Appointments and exact partial date, time and title fields are persisted together
in `/prefs/calendar.v1`. The versioned, bounded binary file includes validation
and a checksum; it is not a text interchange format. Unsupported or externally
changed content is preserved rather than silently replaced.

The status line distinguishes queued private changes, changes staged in filesystem
RAM, confirmed disk saves, and errors. Ordinary edits queue storage work while
remaining responsive. A busy disk or failed write keeps the latest accepted
appointments and unfinished entry in memory for retry, even after closing
Calendar. Filesystem progress and Calendar's staging continue while its window
is closed. Retry synchronizes explicitly; normal desktop autosave also persists
staged changes.

Shutdown drains any older storage operation, stages Calendar's latest agenda and
draft, and then requires the desktop's final disk synchronization before powering
off. A staging or synchronization failure pauses shutdown. A successful save or
successful normal shutdown restores the draft on reboot, including invalid or
incomplete entry text. As elsewhere in BaseOS, unsynchronized RAM changes cannot
survive abrupt power loss. The visible status is the durability claim.

Calendar stores local wall-clock dates and times. It has no recurrence, alerts,
accounts, network synchronization, time-zone conversion or shared calendars.

## Verification

`python3 -m unittest discover -s tests -p 'test_calendar_ui.py'` runs ordinary
mouse/keyboard workflows with the actual model, binary codec, RTC date logic and
software renderer. A bounded mock filesystem tests UI handling of busy and failed
synchronization. It covers close/reopen, staged reboot recovery, leap dates,
invalid time/date correction, deletion/discard confirmation, paging, capacity,
long fields and narrow-window fallback. These host checks are separate from
production QEMU input/reboot verification and the model's filesystem tests.
