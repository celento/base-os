#include "document_save.h"
#include "kernel_owner.h"
#include "native_sync.h"
#include "fs.h"
#include "../sdk/baseos_abi.h"

int document_save_ensure_owner(DocumentSave *save) {
    if (save->owner) return 1;
    save->owner = kernel_owner_allocate();
    if (!save->owner) save->result = BOS_E_CAPACITY;
    return save->owner != 0;
}
int document_target_capture(int file, unsigned length, DocumentTarget *out) {
    DocumentTarget target = {fs_incarnation(), fs_identity(file), fs_content_revision(file), length, file};
    if (!target.incarnation || !target.identity || !target.version || !fs_valid(file) ||
        fs_is_dir(file) || fs_is_app(file) || fs_size(file) != (int)length) return 0;
    *out = target; return 1;
}
int document_target_matches(const DocumentTarget *target) {
    return target->incarnation && target->identity && target->version &&
           target->incarnation == fs_incarnation() && fs_valid(target->file) &&
           !fs_is_dir(target->file) && !fs_is_app(target->file) &&
           fs_identity(target->file) == target->identity &&
           fs_content_revision(target->file) == target->version &&
           fs_size(target->file) == (int)target->length;
}
int document_save_poll(DocumentSave *save) {
    if (!save->pending) return 0;
    int result = native_sync_poll(save->owner, save->handle);
    if (result == BOS_PENDING) return 0;
    (void)native_sync_release(save->owner, save->handle);
    save->pending = 0;
    save->result = result == BOS_OK && !document_target_matches(&save->target) ? BOS_E_STALE : result;
    return 1;
}
int document_save_begin(DocumentSave *save, unsigned kind, DocumentRevision revision,
                        int file, unsigned length, unsigned option) {
    if (save->pending) return DOCUMENT_SAVE_PENDING;
    if (!document_save_ensure_owner(save)) return DOCUMENT_SAVE_ERROR;
    save->handle = 0; save->kind = kind; save->revision = revision; save->option = option;
    if (!document_target_capture(file, length, &save->target)) {
        save->result = BOS_E_STALE; return DOCUMENT_SAVE_ERROR;
    }
    if (!fs_sync_async_supported()) {
        save->result = fs_sync() < 0 ? BOS_E_IO : BOS_OK;
        if (save->result == BOS_OK && !document_target_matches(&save->target)) save->result = BOS_E_STALE;
        return save->result == BOS_OK ? DOCUMENT_SAVE_OK : DOCUMENT_SAVE_ERROR;
    }
    save->result = native_sync_begin(save->owner, &save->handle);
    if (save->result != BOS_OK) return DOCUMENT_SAVE_ERROR;
    save->pending = 1; save->result = BOS_PENDING;
    (void)document_save_poll(save);
    return save->pending ? DOCUMENT_SAVE_PENDING : save->result == BOS_OK ? DOCUMENT_SAVE_OK : DOCUMENT_SAVE_ERROR;
}
void document_save_detach(DocumentSave *save) {
    if (save->owner) native_sync_owner_release(save->owner);
    *save = (DocumentSave){0};
}
const char *document_save_error(const DocumentSave *save) {
    switch (save->result) {
    case BOS_E_PROTECTED: return "Disk is protected. RAM copy retained; retry after recovery.";
    case BOS_E_CAPACITY: return "Save capacity unavailable. RAM copy retained; retry shortly.";
    case BOS_E_BUSY: return "Disk is saving. RAM copy retained; retry shortly.";
    case BOS_E_STALE: return "Save target changed. Work retained; choose a new name.";
    default: return "Disk sync failed. RAM copy retained; retry.";
    }
}
