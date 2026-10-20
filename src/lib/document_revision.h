#ifndef DOCUMENT_REVISION_H
#define DOCUMENT_REVISION_H

/* Private model identity. Zero is reserved for an unknown durable baseline.
 * Allocation fails before mutation at terminal exhaustion; neither component
 * ever wraps into an identity previously issued in this model's lifetime. */
typedef struct { unsigned epoch, counter; } DocumentRevision;
#define DOCUMENT_REVISION_NONE ((DocumentRevision){0, 0})
static inline int document_revision_equal(DocumentRevision a, DocumentRevision b) {
    return a.epoch == b.epoch && a.counter == b.counter;
}
static inline int document_revision_next(DocumentRevision *last, DocumentRevision *out) {
    DocumentRevision next = *last;
    if (next.counter == ~0u) {
        if (next.epoch == ~0u) return 0;
        ++next.epoch; next.counter = 1;
    } else {
        if (!next.epoch) next.epoch = 1;
        ++next.counter;
    }
    *last = next; *out = next; return 1;
}
#endif
