#ifndef BASEOS_DOWNLOAD_H
#define BASEOS_DOWNLOAD_H
#include "fs.h"
#include "net.h"

/* One background transfer, independent of any Terminal window. The desktop
 * calls download_tick immediately after net_poll, outside borrowed fs_data
 * lifetimes. No download function waits for network or disk I/O. */
enum { DOWNLOAD_IDLE, DOWNLOAD_ACTIVE, DOWNLOAD_DONE,
       DOWNLOAD_ERROR, DOWNLOAD_CANCELLED };
typedef struct {
    int state, http_state, http_status, file_id;
    unsigned request_id, received, limit;
    char url[NET_URL_MAX], path[FS_PATH_LEN], message[160];
} DownloadStatus;

void download_init(void);
/* 0 accepted; -1 rejected. Existing files and active jobs are preserved.
 * Explicit http:// URLs only; redirects are reported but never followed. */
int download_start(int cwd, const char *url, const char *path);
int download_tick(void); /* nonzero when observable status/files changed */
int download_cancel(void); /* only cancels this service's own HTTP request */
int download_active(void);
const DownloadStatus *download_status(void);
const char *download_last_error(void); /* last rejected start, not active status */
#endif
