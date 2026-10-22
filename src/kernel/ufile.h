#ifndef UFILE_H
#define UFILE_H
#include <stdint.h>

/* File descriptors for ELF processes (platform ABI v2). Descriptors name
 * shared, reference-counted open files: the console, native volume files and
 * directories, and pipes. Native files are whole-file atomic on the volume, so
 * a writable open file keeps its contents in a kernel buffer and writes them
 * back on fsync or on its last close. Called from system calls on the
 * process's own thread, except ufile_table_close_all and ufile_poll. */

struct Uproc;
struct UFile;

typedef struct {
    struct UFile **slots;      /* grows on demand */
    unsigned capacity;
    int cwd;                   /* node of the working directory */
    unsigned cwd_identity;     /* fs_identity of cwd when it was set */
    char *cwd_path;            /* working directory on a FAT volume, else 0 */
} UFileTable;

/* Descriptors 0, 1 and 2 on the console. 0 or -BOS_ENOMEM. */
int ufile_table_init(UFileTable *table);
/* Writes back every changed file, waiting while the volume is busy. */
void ufile_table_flush(UFileTable *table);
/* Drops every descriptor. Unsaved file contents are written back now, or by
 * ufile_poll once the volume accepts writes again. */
void ufile_table_close_all(UFileTable *table);
/* Kernel loop: retries write-backs that the volume refused while busy. */
void ufile_poll(void);

int ufile_open(struct Uproc *process, uint32_t path, int flags, int mode);
int ufile_close(struct Uproc *process, int fd);
int ufile_read(struct Uproc *process, int fd, uint32_t buffer, uint32_t length);
int ufile_write(struct Uproc *process, int fd, uint32_t buffer, uint32_t length);
int ufile_seek(struct Uproc *process, int fd, uint32_t low, uint32_t high, int whence, uint32_t result);
int ufile_stat(struct Uproc *process, uint32_t path, uint32_t out);
int ufile_fstat(struct Uproc *process, int fd, uint32_t out);
int ufile_isatty(struct Uproc *process, int fd);
int ufile_readdir(struct Uproc *process, int fd, uint32_t buffer, uint32_t length);
int ufile_mkdir(struct Uproc *process, uint32_t path);
int ufile_unlink(struct Uproc *process, uint32_t path, int flags);
int ufile_rename(struct Uproc *process, uint32_t from, uint32_t to);
int ufile_dup(struct Uproc *process, int fd);
int ufile_dup2(struct Uproc *process, int fd, int target);
int ufile_pipe(struct Uproc *process, uint32_t fds);
int ufile_chdir(struct Uproc *process, uint32_t path);
int ufile_getcwd(struct Uproc *process, uint32_t buffer, uint32_t length);
int ufile_fsync(struct Uproc *process, int fd);
int ufile_truncate(struct Uproc *process, int fd, uint32_t length);
#endif
