# Settings preferences during disk snapshots

Appearance, screen-saver and confirmed display choices apply to the desktop
immediately. Settings keeps the latest choice for each of `/prefs/theme`,
`/prefs/saver` and `/prefs/display` while an asynchronous snapshot leases the
filesystem. Later choices replace earlier queued choices; a display preview
only becomes a preference when the user selects Keep. Escape and the existing
15-second timeout still restore the prior resolution and window geometry.

`preferences_tick()` runs immediately after the top-level storage pump. It
observes completed saves before making any queued mutations, then writes at
most three one-byte preferences after the lease releases. It never pumps the
snapshot, dispatches input or borrows filesystem data across calls.

## Feedback and retry

Each section has its own status. A queued choice, a preference written only to
RAM, and a durably saved preference have distinct messages. `Resolution saved`
and `Preference saved` require a clean, successfully synchronized filesystem
and matching current destination identity/content. A write returning one byte
is not proof of disk durability. Storage failures/protected mounts remain
visible as RAM-only failures; occupied destinations and full volumes have
separate error messages. Applying a choice does not erase another section's
error.

Normal saves use the existing autosync backoff and three-failure limit. Failed
writes retain their latest choice but do not repeatedly allocate or write on
every desktop tick. The **Retry saving (R)** button/key retries failed choices
and requests an asynchronous snapshot explicitly, including after automatic
retries are exhausted. It retains and releases its own result ticket; a foreign
retained result delays this admission to at most one request per second. A new
choice made during that retry is queued for the next save. Repeatedly selecting
the same value does not rewrite an unchanged file. Successful autosync or an
explicit save can also establish durability after an earlier failure.

The status rows and retry control fit the existing 560 × 458 Settings geometry.
No window minimum or display-confirmation timing is changed.

For Shutdown, `preferences_prepare_shutdown()` replaces the initial busy join.
It drains that lease, reaps any preference retry ticket and attempts retained
choices once. It returns failure for an unwritten preference, keeping the
desktop available. Success only means the preferences are ready in RAM; the
caller must still save the session and complete its final `fs_sync()` before
poweroff. This prevents a confirmed choice queued behind an older snapshot
from being silently omitted during shutdown.

## Fixed paths and storage protection

Preference paths are resolved from the root on each write. The cached folder ID
cannot redirect a queued preference into a moved or reused directory. An
existing `/prefs` must be a directory; a preference must be an ordinary file
containing exactly one recognized digit. Directories, application nodes and
unrecognized file contents are left alone. Full-volume/node capacity is checked
before creating even an empty preference file or directory. A same-size update
does not compact unrelated file data.

RAM-only changes on a protected/unknown volume do not grant permission to write
that disk, reformat it or bypass filesystem admission. Manual retry uses the
normal sync API and retains the same protection. No additional preference path,
wire format, snapshot arena or persistent metadata is introduced.

## Deterministic checks

Run `PYTHONPATH=tests python3 -m unittest test_settings_preferences` with an
ordinary host C compiler. The ASan/UBSan harness extracts the actual production
preference helpers and display Keep/request/revert/load functions, links them
to the real filesystem and a deterministic in-memory block device, and checks:

- Busy → release, latest choice wins, no writes during the lease, and no repeated
  writes on 1,000 unchanged polls.
- Keep versus Revert, the 15-second deadline, and rebooted preference values.
- Fresh fixed-path resolution, stale cached IDs, directory/app/unrecognized-file
  rejection, destination changes before durability and retained confirmed choices.
- Full byte/node preflight without partial preference creation and explicit retry.
- Ordinary transport write failure, autosync backoff/exhaustion, successful retry,
  unknown-disk/read-only protection and foreign result-slot backoff.
- Shutdown during autosync/manual retry, retained confirmed choices, and refusal
  to finish after write/sync failure.
- Legacy floppy saves and observing an older preference's clean completion
  before a queued newer section makes the filesystem dirty again.

These are ordinary deterministic workflows and I/O errors, with no intentional
memory faults or fuzzing. QEMU screenshot/input validation is a separate check.
