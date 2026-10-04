#ifndef BASEOS_LARGER_MEMORY_APP_SHIM_H
#define BASEOS_LARGER_MEMORY_APP_SHIM_H
#define BASEOS_APP2_H
#include "../sdk/baseos_abi.h"
void *bos_workspace(void);
unsigned bos_workspace_bytes(void);
unsigned bos_task_id(void);
int bos_argument(char *out,unsigned capacity);
int bos_memory_info(BosMemoryInfo *out,unsigned capacity);
int bos_file_open(const char *path,unsigned flags,BosFileInfo *out);
int bos_file_replace(BosHandle file,const void *data,unsigned bytes,BosFileInfo *out);
int bos_file_close(BosHandle file);
int bos_sync_begin(BosHandle *out);
int bos_sync_wait(BosHandle operation,unsigned milliseconds);
int bos_sync_release(BosHandle operation);
int bos_yield(void);
int bos_sleep(unsigned milliseconds);
int bos_key(void);
int bos_write(const char *text,unsigned bytes);
int bos_print(const char *text);
#endif
