#ifndef NATIVE_FILES_H
#define NATIVE_FILES_H

#include <stdint.h>
#include "../sdk/baseos_abi.h"

#define NATIVE_FILE_CAPACITY 64u
#define NATIVE_FILE_PER_OWNER 8u
#define NATIVE_FILE_READ_MAX 4096u
#define NATIVE_FILE_REPLACE_MAX 32768u
#define NATIVE_FILE_PATH_MAX 128u

/* Serialized native services. The dispatcher validates all user memory and
 * provides the generation-qualified process owner. No pointer is retained.
 * init invalidates all handles, but never resets the nonreused serial counter. */
void native_files_init(void);
void native_files_release_owner(uint32_t owner);
int native_file_open(uint32_t owner, const char *path, uint32_t flags, BosFileInfo *out);
int native_file_info(uint32_t owner, uint32_t handle, BosFileInfo *out);
int native_file_read_at(uint32_t owner, uint32_t handle, uint32_t offset,
                        void *out, uint32_t capacity);
/* Atomic RAM replacement, not durable sync. Success updates this handle's
 * version and copied metadata. Other handles retain their old version. */
int native_file_replace(uint32_t owner, uint32_t handle, const void *data,
                        uint32_t length, BosFileInfo *out);
int native_file_close(uint32_t owner, uint32_t handle);

#define NATIVE_FILE_TRANSACTION_MAX 262144u
#define NATIVE_FILE_TRANSACTION_CHUNK 4096u
#define NATIVE_FILE_TRANSACTION_CAPACITY 8u
#define NATIVE_FILE_TRANSACTION_PER_OWNER 1u
#define NATIVE_FILE_TRANSACTION_PAGES 64u
#define NATIVE_FILE_TRANSACTION_TOTAL_PAGES 128u
/* These methods receive only copied/validated kernel inputs. BEGIN path is a
 * copied terminated path corresponding exactly to input.path_bytes, never an
 * app pointer. No source buffer survives APPEND. Serialized calls only. */
int native_file_transactions_available(void);
void native_file_transactions_tick(void);
void native_file_transactions_query(BosFileTransactionInfoV1 *out);
int native_file_transaction_begin(uint32_t owner, unsigned mode,
    const BosFileTransactionBeginV1 *input, const char *path, BosFileTransactionStatusV1 *out);
int native_file_transaction_append(uint32_t owner, uint32_t handle, const void *data,
                                   unsigned length, unsigned offset);
int native_file_transaction_info(uint32_t owner, uint32_t handle, BosFileTransactionStatusV1 *out);
int native_file_transaction_accept(uint32_t owner, uint32_t handle, BosFileInfo *out);
int native_file_transaction_abort(uint32_t owner, uint32_t handle);

#endif
