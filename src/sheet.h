#ifndef BASEOS_SPREADSHEET_H
#define BASEOS_SPREADSHEET_H
#include "sheet_codec.h"
#include "document_save.h"
#define SPREADSHEET_W 720
#define SPREADSHEET_H 520
#define SPREADSHEET_MIN_W 360
#define SPREADSHEET_MIN_H 260
#define SPREADSHEET_MOD_CTRL 1
#define SPREADSHEET_MOD_SHIFT 2
#define SPREADSHEET_MOD_ALT 4
#define SPREADSHEET_CHANGED 1
#define SPREADSHEET_REQUEST_SAVE 2
#define SPREADSHEET_REQUEST_SAVE_AS 4
#define SPREADSHEET_REQUEST_EXPORT 8
#define SPREADSHEET_SAVE_ERROR (-1)
#define SPREADSHEET_SAVE_NEEDS_NAME 0
#define SPREADSHEET_SAVE_OK 1
#define SPREADSHEET_SAVE_PENDING 2
#define SPREADSHEET_EXPORT_PENDING (-2)
#define SPREADSHEET_CLIPBOARD_CAPACITY 65536u
/* Singleton client-area app, independent of the model's sheet_init(). The
 * desktop owns Save/Discard/Cancel guards before new/open/close, filename
 * dialogs and recovery sidecars. Coordinates include the client origin. */
void spreadsheet_init(void);
void spreadsheet_new(void);
int spreadsheet_open_file(int id);
void spreadsheet_draw(int x, int y, int w, int h);
/* Ctrl+1..4 applies General/Fixed2/Currency/Percent to the selection.
 * Ctrl+- / Ctrl+= adjusts selected columns by 16px within 48..320; Ctrl+0
 * restores 104px. Metadata controls commit pending source first and are undoable. */
int spreadsheet_key(int scancode, char character, int modifiers);
int spreadsheet_click(int x, int y, int w, int h, int mx, int my, int modifiers);
int spreadsheet_drag(int x, int y, int w, int h, int mx, int my);
void spreadsheet_release(void);
int spreadsheet_scroll(int lines);
int spreadsheet_tick(void);
void spreadsheet_close(void);
/* IDE durability is asynchronous. Pending is never SAVE_OK; edits can continue.
 * The desktop polls once after storage service, independently of blink/edit mode.
 * Active saves/exports and temporary storage leases reject new writes before
 * committing cell edits. A duplicate native Save returns its existing pending. */
int spreadsheet_persistence_poll(void);
void spreadsheet_save_info(DocumentSave *out);
int spreadsheet_save(void);
int spreadsheet_save_as(int parent, const char *name);
/* New .csv name, or exact owned failed-export retry; calculated values only.
 * No native rebind or dirty reset.
 * Formula-like text is verbatim and other spreadsheet programs may execute it.
 * Returns durable filesystem ID, SPREADSHEET_EXPORT_PENDING, or -1.
 * Failed durability retains complete RAM output for an explicit same-name retry. */
int spreadsheet_export_csv(int parent, const char *name);
/* Exact owned retry name for a still-matching document/target. No polling,
 * mutation or truncation; returns zero without changing out when unavailable. */
int spreadsheet_export_retry_name(char *out, unsigned capacity);
const char *spreadsheet_title(void);
const char *spreadsheet_status(void);
int spreadsheet_dirty(void);
int spreadsheet_file(void);
unsigned spreadsheet_file_identity(void);
typedef struct { unsigned size, hash_a, hash_b; } SpreadsheetBinding;
int spreadsheet_binding(SpreadsheetBinding *out);
int spreadsheet_binding_matches(int file, const SpreadsheetBinding *binding);
/* Recovery source is native bytes, borrowed until the next app operation.
 * snapshot includes pending cell bytes without interrupting the live editor;
 * an invalid candidate returns NULL. Save/export commit pending edits.
 * Caller verifies persisted baseline fingerprint before passing a native file
 * to restore; otherwise pass -1/0 and restore keeps the draft unbound/dirty. */
const unsigned char *spreadsheet_snapshot(unsigned *length);
int spreadsheet_restore(const unsigned char *data, unsigned length, int file,
                        unsigned identity, int dirty, unsigned caret, unsigned anchor);
/* Row-major indices (0..3327); caret is active corner, anchor fixed corner. */
unsigned spreadsheet_caret(void);
unsigned spreadsheet_anchor(void);
const SheetDoc *spreadsheet_document(void);
int spreadsheet_editing(void);
/* Geometry of a visible cell, relative to client origin; 1 visible, 0 hidden.
 * Call draw once after resize before querying. No framebuffer access. */
int spreadsheet_cell_position(unsigned row, unsigned col, int *x, int *y, int *w, int *h);
unsigned spreadsheet_first_row(void);
unsigned spreadsheet_first_col(void);
/* Complete shared text, never silently truncated. Successful set returns a
 * nonzero generation changing on EVERY write, including identical bytes.
 * set returning zero rejects the copy without changing shared clipboard bytes. get returns
 * full byte length (no required NUL) or -1 for unavailable/oversize. Self-owned
 * copies preserve kinds/sources/formats in a private 64KiB bounded record; external
 * TSV imports numeric fields as NUMBER and all other fields as literal TEXT.
 * Column widths stay at the destination. External TSV uses formatted display
 * values; symbols/percent signs import as literal text. Relative formula
 * references are NOT rewritten on copy/paste. */
unsigned spreadsheet_clipboard_set(const char *text, unsigned length);
int spreadsheet_clipboard_get(char *text, unsigned capacity, unsigned *generation);
#endif
