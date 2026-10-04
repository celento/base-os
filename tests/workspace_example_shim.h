#ifndef BASEOS_WORKSPACE_EXAMPLE_SHIM_H
#define BASEOS_WORKSPACE_EXAMPLE_SHIM_H
/* Host-only normal service model; the real app algorithm is compiled unchanged.
 * Skip interrupt wrappers and linker-pointer access while retaining ABI types. */
#define BASEOS_APP2_H
#include "../sdk/baseos_abi.h"
#define BOS_ARGUMENT_MAX 128u
#define BOS_FILE_CHUNK_MAX 4096u
void *bos_workspace(void);
unsigned bos_workspace_bytes(void);
unsigned bos_task_id(void);
int bos_argument(char *out,unsigned capacity);
int bos_file_open(const char *path,unsigned flags,BosFileInfo *out);
int bos_file_read_at(BosHandle file,void *out,unsigned capacity,unsigned offset);
int bos_file_replace(BosHandle file,const void *data,unsigned bytes,BosFileInfo *out);
int bos_file_close(BosHandle file);
int bos_sync_begin(BosHandle *out);
int bos_sync_wait(BosHandle operation,unsigned milliseconds);
int bos_sync_release(BosHandle operation);
int bos_yield(void);
int bos_sleep(unsigned milliseconds);
int bos_print(const char *text);
int bos_write(const char *text,unsigned bytes);
#endif
