#ifndef BASEOS_FILE_CLIPBOARD_H
#define BASEOS_FILE_CLIPBOARD_H

#include "fs.h"

enum {
    FILE_CLIPBOARD_NONE,
    FILE_CLIPBOARD_COPY,
    FILE_CLIPBOARD_CUT
};

enum {
    FILE_CLIPBOARD_ERROR = -1, /* No new file or move retained. */
    FILE_CLIPBOARD_NOOP = 0,   /* Same-folder Cut; clipboard consumed. */
    FILE_CLIPBOARD_SYNCED = 1, /* Completed operation, disk sync confirmed. */
    FILE_CLIPBOARD_RAM_ONLY = 2 /* Completed in RAM; disk sync failed. */
};

#define FILE_CLIPBOARD_STATUS_LEN 128

/* Copy an ordinary file/folder in RAM with a collision-safe, extension-aware
 * name. Keeps an unused original name; otherwise makes "report copy.bwr",
 * "report copy 2.bwr", etc. Folders retain fs_unique_copy naming.
 * Returns the new node or -1 with no copy retained. Never syncs and leaves ALL
 * clipboard selection, status and retry state unchanged. Callers own sync.
 * Same-folder use implements Duplicate while preserving file associations. */
int file_copy_named(int source, int directory);

/* One desktop-wide file clipboard. A Copy is a reference to the live source,
 * not a frozen snapshot. Its node ID AND fs_identity must still match. */
void file_clipboard_clear(void);
/* 0 accepted, -1 rejected. Rejected selections preserve the old clipboard.
 * Root, app nodes and folders containing app nodes are not ordinary sources. */
int file_clipboard_set(int source, int mode);
int file_clipboard_mode(void);
int file_clipboard_source(void); /* -1 if empty, deleted or replaced. */
unsigned file_clipboard_identity(void);
const char *file_clipboard_name(void); /* Current name, or last known name. */

/* Structural availability only; capacity/path limits and disk I/O are checked
 * by paste. Has no side effects and does not replace the last status message. */
int file_clipboard_can_paste(int directory);
/* Always initializes *result_node when non-NULL. It identifies a completed
 * result in this directory, including RAM_ONLY, or -1 if none is selectable.
 * Cut is consumed after its RAM move, even when sync fails. Copy stays live.
 * Copy keeps the source name if unused in the destination; otherwise it uses
 * file_copy_named's extension-aware collision-safe name.
 * After Copy returns RAM_ONLY, the next Paste only retries that completed
 * operation's sync, even if autosync succeeded meanwhile. No duplicate copy is
 * made. A fresh Copy/Cut or clear explicitly ends this retry guard.
 * RAM_ONLY must be reported as "changed in RAM", never "nothing changed". */
int file_clipboard_paste(int directory, int *result_node);
/* A completed paste awaits a confirmed sync through this module. This can
 * remain true after filesystem autosync until Paste consumes the retry guard. */
int file_clipboard_pending_sync(void);
const char *file_clipboard_status(void); /* NUL-terminated, < STATUS_LEN bytes. */

/* Call clear whenever new TEXT is written to the shared desktop clipboard.
 * Clearing does not undo any already completed filesystem operation. */

#endif
