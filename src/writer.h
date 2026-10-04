#ifndef BASEOS_WRITER_H
#define BASEOS_WRITER_H
#include "writer_codec.h"
#include "writer_pdf.h"
#define WRITER_W 720
#define WRITER_H 520
#define WRITER_MIN_W 420
#define WRITER_MIN_H 260
#define WRITER_MOD_CTRL 1
#define WRITER_MOD_SHIFT 2
#define WRITER_MOD_ALT 4
#define WRITER_CHANGED 1
#define WRITER_REQUEST_SAVE 2
#define WRITER_REQUEST_SAVE_AS 4
#define WRITER_REQUEST_EXPORT 8
#define WRITER_REQUEST_PDF 16
#define WRITER_SAVE_ERROR (-1)
#define WRITER_SAVE_NEEDS_NAME 0
#define WRITER_SAVE_OK 1
/* Singleton, independent of Editor. Coordinates include supplied client origin.
 * Caller must perform its Save/Discard/Cancel guard before new/open/close. */
void writer_init(void);
void writer_new(void);
int writer_open_file(int id);
void writer_draw(int x, int y, int w, int h);
int writer_key(int scancode, char character, int modifiers);
int writer_click(int x, int y, int w, int h, int mx, int my, int modifiers);
int writer_drag(int x, int y, int w, int h, int mx, int my);
void writer_release(void);
int writer_scroll(int lines);
int writer_tick(void);
void writer_close(void);
int writer_save(void);
/* New names only, except the currently bound, identity-matching native file.
 * Existing unrelated names are rejected rather than silently overwritten. */
int writer_save_as(int parent, const char *name);
/* Creates a new .rtf export; never rebinds or clears the native dirty marker.
 * Returns the new nonnegative filesystem ID on success, -1 on failure. */
int writer_export_rtf(int parent, const char *name);
/* New .pdf export on Letter or A4. Exact size/storage checks precede creation.
 * Never rebinds/clears native state. A disk-sync failure retains our RAM file;
 * retrying the same name/paper only syncs if identity, revision and bytes match.
 * Returns a synchronized file ID, or -1 with writer_status() explaining why. */
int writer_export_pdf(int parent, const char *name, unsigned paper);
const char *writer_title(void);
const char *writer_status(void);
int writer_dirty(void);
int writer_read_only(void);
int writer_file(void);
unsigned writer_file_identity(void);
/* Cached baseline bytes for the bound native file; not the current dirty draft.
 * Persist this compact record in a versioned sidecar with the saved path.
 * Identity checks remain necessary during a boot. Two noncryptographic hashes
 * detect ordinary external replacements across reboot; this is not an auth tag.
 * binding() returns 1 when a cached bound baseline exists, else 0. matches()
 * returns 1 only for an available native file with identical size/hash bytes.
 * Both leave output/state unchanged on failure; matches is read-only and polls. */
typedef struct { unsigned size, hash_a, hash_b; } WriterBinding;
int writer_binding(WriterBinding *out);
int writer_binding_matches(int file, const WriterBinding *binding);
unsigned writer_length(void);
unsigned writer_word_count(void);
unsigned writer_caret(void);
unsigned writer_anchor(void);
/* Recovery is native-format bytes; autosaving does not mark the document saved.
 * Snapshot pointer is borrowed until next Writer operation. Restore validates
 * before replacing; binding only accepted for matching native file identity. */
const unsigned char *writer_snapshot(unsigned *length);
int writer_restore(const unsigned char *data, unsigned length, int file,
                   unsigned identity, int dirty, unsigned caret, unsigned anchor);
/* Plain shared clipboard hooks: desktop overrides weak defaults. A successful
 * set returns the resulting generation. Generation must change on EVERY write,
 * including writes containing identical bytes. get returns complete length or
 * -1 if unavailable/too large (never a silently truncated clipboard). */
unsigned writer_clipboard_set(const char *text, unsigned length);
int writer_clipboard_get(char *text, unsigned capacity, unsigned *generation);
/* Deterministic geometry/model access for functional tests and integration. */
const WriterDoc *writer_document(void);
unsigned writer_line_count(void);
int writer_position(unsigned index, int *x, int *y, int *height);
unsigned writer_hit_position(int x, int y);
void writer_layout(int width);
#endif
