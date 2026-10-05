# Conditional file transactions (candidate ABI 1.3)

This candidate adds a desktop IDE service for privately uploading and atomically
publishing up to 256 KiB. It is synchronous RAM acceptance, not an implicit disk
save or a scheduling/latency guarantee. The existing whole-buffer replacement
limit remains 32 KiB; built-in document models, BEX1/BEX2 formats and disk formats
are unchanged. General ABI is 1.3; the independently negotiated UI stays 1.1
and Close/Stop remains forced. The standalone integration is **UNQUALIFIED**
until archived-binary compatibility, near-full placement and its own final-source
profile boots pass. Guest evidence from the separately pinned source runtime
does not establish binary identity for this integration.

Use `bos_file_transaction_query` to negotiate support. It returns UNSUPPORTED on
old runtimes, legacy synchronous execution and floppy; never truncate or silently
fall back to an unconditional path write. Protected IDE remains discoverable but
refuses BEGIN/ACCEPT with PROTECTED. Limits and usage snapshots reserve nothing.
There is one stage per process, eight global records, 64 private 4-KiB pages
per maximal stage and 128 pages globally. Thus two simultaneous maximal stages
fit the global page budget; smaller stages can share the remaining records.
The complete call-30 register contract is in the
[platform ABI guide](NATIVE_PLATFORM_ABI.md#conditional-staged-publication-call-30),
with exact wire layouts in `sdk/baseos_abi.h`.

1. Use `bos_file_transaction_begin_replace(file, revision, total, &status)` for
   an open writable file and an explicitly selected current revision, or
   `bos_file_transaction_begin_create(path, total, &status)` for an absent leaf.
   Creation remains unpublished until acceptance; no placeholder file is made.
2. Append copied chunks of 1..4096 bytes at exactly `received_bytes`. Reuse the
   source buffer after each return. A zero-byte document is complete at BEGIN.
3. Call `bos_file_transaction_accept_ram(stage, &file_info)` after all bytes are
   uploaded. Success consumes the stage and returns the updated/new file handle
   and content revision. Other readers retain their old bound revision and
   report CHANGED when they next read or inspect that handle.
4. Request a separate owned sync and, after success, compare FILE_INFO on that
   same handle to the captured accepted revision. The explicit
   `bos_file_sync_revision` helper performs that protocol. It proves only those
   content bytes, not a subsequently changed pathname or other metadata.

A same-incarnation BUSY/CAPACITY/conflict leaves the stage and app model intact.
BUSY can be retried after yielding. A conflict is not permission to overwrite a
new version. Abort, deliberately reopen/reupload a chosen newer version, or
explicitly create a different Save As path. There is no stage rebase or retarget.
Closing the source file aborts its replacement stage; exit/Stop/reset release
all exact owner stages. Success already accepted into RAM survives owner exit.

Mount invalidation releases stage pages and retains INVALIDATED metadata with
zero received bytes. INFO/ABORT remain available to desktop callers even after
an IDE-to-floppy transition removes discovery support; APPEND/ACCEPT return
CHANGED. ABORT succeeds once. Foreign/consumed handles return STALE. No private
upload holds the global snapshot/image arena or a snapshot lease.

Full registry, record layouts, contextual error precedence and review decisions
are in [the frozen decisions](proposals/native-file-transactions-2026-10-05/DECISIONS.md).

## Streaming document example

`examples/c/staged_document.c` keeps a compact deterministic document model and
one reusable 4 KiB buffer. Build optional fixtures with:

```
make build/staged-document.bex build/staged-document2.bex build/staged-document-window.bex
```

These are hosted BEX1, hosted BEX2 and owned-window BEX2 versions of the same
source. They are not added to the installed example catalogue. The default path
is `/Documents/staged.txt`; an ordinary startup document argument overrides it.

- C stages an unpublished create at `/Documents/created.txt`; R explicitly selects the current version of
  the existing path and stages replacement. Upload yields between chunks.
- A accepts a complete stage into RAM. The display distinguishes STAGED from
  RAM ACCEPTED and CONTENT DURABLE.
- D obtains a new owned receipt and confirms its captured accepted content
  revision after completion. An edited private model stays visibly UNSAVED even
  if an older accepted revision becomes durable.
- V coherently rereads and compares every byte of the accepted version.
- M changes the compact private model; S explicitly aborts any old stage and
  reuploads that preserved model under a fresh task-qualified Save As name.
- Q explicitly discards any private stage and exits. A forced shell Stop relies
  on normal owner cleanup. This fixture does not claim save-aware Close support.

Run two instances against the same path, stage both, accept one, then accept the
other to see a real conditional conflict. S is explicit conflict recovery. Output
logs include STAGED, RAM_ACCEPTED, DURABLE_CONFIRMED and VERIFY_EXACT records for
ordinary external-input guest observation. PIT ticks are never used to claim
acceptance elapsed time. Source-equivalent guest evidence and this integration's
own final-source boots remain separately identified in the integration ledger;
neither host results nor design documents waive the remaining qualification gates.
