#ifndef HISTORY_H
#define HISTORY_H
/* Caller owns capacity * item_size bytes. Swap-based history keeps redo
 * without allocating another full copy of the current document. */
typedef struct { unsigned count, cursor, capacity, size; unsigned char *items; } History;
void history_record(History *h, const void *current);
int history_step(History *h, void *current, int redo);
#endif
