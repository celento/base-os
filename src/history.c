#include "history.h"
#include "fs.h"
void history_record(History *h, const void *current) {
    if (!h->capacity || !h->size) return;
    h->count = h->cursor;
    if (h->count == h->capacity) {
        /* Forward copy is safe when shifting storage left. */
        kmemcpy(h->items, h->items + h->size, (h->count - 1) * h->size);
        h->count--;
    }
    kmemcpy(h->items + h->count * h->size, current, h->size);
    h->cursor = ++h->count;
}
int history_step(History *h, void *current, int redo) {
    if ((!redo && !h->cursor) || (redo && h->cursor == h->count)) return 0;
    unsigned index = redo ? h->cursor++ : --h->cursor;
    unsigned char *a = current, *b = h->items + index * h->size;
    for (unsigned i = 0; i < h->size; i++) { unsigned char t = a[i]; a[i] = b[i]; b[i] = t; }
    return 1;
}
