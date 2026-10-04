#ifndef DOCUMENT_SAVE_H
#define DOCUMENT_SAVE_H
#include "document_revision.h"

#define DOCUMENT_SAVE_ERROR (-1)
#define DOCUMENT_SAVE_OK 1
#define DOCUMENT_SAVE_PENDING 2
#define DOCUMENT_EXPORT_PENDING (-2)
enum DocumentSaveKind { DOCUMENT_SAVE_NATIVE = 1, DOCUMENT_SAVE_RTF,
                        DOCUMENT_SAVE_PDF, DOCUMENT_SAVE_CSV };
typedef struct {
    unsigned incarnation, identity, version, length;
    int file;
} DocumentTarget;
/* Value-only record, embedded by each singleton. owner is the logical document
 * lifetime; handle identifies one accepted boundary (zero for compatibility or
 * rejected requests). result is BOS_* and must never be returned as SAVE_OK. */
typedef struct {
    unsigned owner, handle, kind, option;
    int pending, result;
    DocumentRevision revision;
    DocumentTarget target;
} DocumentSave;
_Static_assert(sizeof(DocumentSave) <= 64, "document save record exceeds budget");

/* Metadata-only except for the explicit unsupported-backend floppy branch in
 * begin(). IDE errors never invoke the blocking compatibility wrapper. */
int document_save_ensure_owner(DocumentSave *save);
int document_target_capture(int file, unsigned length, DocumentTarget *out);
int document_target_matches(const DocumentTarget *target);
int document_save_begin(DocumentSave *save, unsigned kind, DocumentRevision revision,
                        int file, unsigned length, unsigned option);
/* Apply/copy a terminal event once. Caller owns model/UI changes. No callbacks. */
int document_save_poll(DocumentSave *save);
void document_save_detach(DocumentSave *save);
const char *document_save_error(const DocumentSave *save);
#endif
