/* Bounded desktop Spreadsheet. Large storage belongs to its reserved arena. */
#include "apps/sheet.h"
#include "kernel/gfx.h"
#include "ui/app.h"
#include "kernel/fs.h"
#include "kernel/layout.h"
#include "kernel/platform.h"
#include "ui/icons.h"
#include "ui/icon_draw.h"
#include "ui/kit.h"
#include "ui/ui_icons.h"
#include "../sdk/baseos_abi.h"

#ifndef SHEET_BASE
#define SHEET_BASE 0x720000
#endif
#ifndef SHEET_CAPACITY
#define SHEET_CAPACITY 0x2E0000
#endif
#define HISTORY 5u
#define ROW_H 24
#define OUTPUT_CAPACITY SHEET_CSV_MAX_SIZE
/* Window-relative layout for the current client size and style: toolbar,
 * formula bar, headers, grid, scrollbars and status bar. geometry()
 * refreshes it before any layout or hit test; drawing and hits share it. */
typedef struct { int x, y, w, h; } Clip;
typedef struct {
    int tool_h, bar_y, bar_h, head_y, head_h, grid_y, bottom, right, row_w, status_y, status_h, sb;
    Clip name, cancel, enter, source;   /* formula bar controls */
} Frame;
static Frame fr;
static void frame_layout(int w, int h);

typedef struct { SheetDoc doc; unsigned caret, anchor; DocumentRevision revision; } Snapshot;
typedef struct {
    int valid, parent;
    unsigned parent_identity;
    DocumentTarget target;
    DocumentRevision revision;
    char name[FS_NAME_LEN];
} SpreadsheetExportRetry;
_Static_assert(sizeof(SpreadsheetExportRetry) == 40u + FS_NAME_LEN, "Spreadsheet export retry budget changed");
typedef struct {
    Snapshot history[HISTORY];
    SheetDoc staging;
    unsigned char output[OUTPUT_CAPACITY];
    unsigned char clipboard[SPREADSHEET_CLIPBOARD_CAPACITY];
} SpreadsheetArena;
_Static_assert(sizeof(SpreadsheetArena) == 2844936u, "Spreadsheet footprint changed");
_Static_assert(sizeof(SpreadsheetArena) <= SHEET_CAPACITY, "Spreadsheet arena overflow");
#ifdef SPREADSHEET_HOST_TEST
static SpreadsheetArena host_arena;
#define ARENA (&host_arena)
#else
#define ARENA ((SpreadsheetArena *)SHEET_BASE)
#endif
static struct {
    unsigned first, count, current;
    DocumentRevision next_revision, saved_revision;
    DocumentSave save;
    SpreadsheetExportRetry export_retry;
    unsigned clipboard_generation, clipboard_length, clipboard_rows, clipboard_cols;
    unsigned clipboard_text_length;
    int clipboard_owned;
    int initialized, file, failed_save, has_binding, binding_conflict;
    unsigned identity, incarnation;
    SpreadsheetBinding binding;
    unsigned first_row, first_col, visible_rows, scroll_x;
    int width, height, dragging, drag_kind, blink, last_blink;
    int inactive, grab;   /* window lacks focus; scrollbar thumb grab offset */
    unsigned click_cell, click_tick;
    int click_valid;
    /* mode: 0 grid, 1 cell editor, 2 A1 jump. Editor bytes remain exact, even
     * when imported TAB/CR/LF are visibly represented by '?' in the bar. */
    int mode, edit_changed;
    SheetKind edit_kind;
    unsigned edit_length, edit_caret, edit_anchor;
    char edit[SHEET_TEXT_MAX + 1u];
    char title[64], status[144];
} state;

/* The weak hooks implement a local-only clipboard. -2 is reserved for that
 * fallback; a real desktop hook returns -1 on failure and a complete length
 * otherwise. Rejected set (zero) must never license Cut or replace ownership. */
#ifndef SPREADSHEET_HOST_EXTERNAL_CLIPBOARD
static unsigned local_clipboard_generation;
__attribute__((weak)) unsigned spreadsheet_clipboard_set(const char *text, unsigned length) {
    (void)text; (void)length;
    if (!++local_clipboard_generation) ++local_clipboard_generation;
    return local_clipboard_generation;
}
__attribute__((weak)) int spreadsheet_clipboard_get(char *text, unsigned capacity, unsigned *generation) {
    (void)text; (void)capacity; *generation = local_clipboard_generation; return -2;
}
#endif
static void copy_bytes(void *out, const void *in, unsigned length) {
    unsigned char *d = out; const unsigned char *s = in;
    for (unsigned i = 0; i < length; i++) { d[i] = s[i]; if ((i & 4095u) == 4095u) platform_poll(); }
}
static unsigned text_length(const char *s) { unsigned n = 0; if (s) while (s[n]) n++; return n; }
static void text_copy(char *out, unsigned capacity, const char *in) {
    unsigned i = 0;
    if (!capacity) return;
    if (in) while (i + 1 < capacity && in[i]) { out[i] = in[i]; i++; }
    out[i] = 0;
}
static void status(const char *message) { text_copy(state.status, sizeof state.status, message); }
static unsigned min_u(unsigned a, unsigned b) { return a < b ? a : b; }
static unsigned max_u(unsigned a, unsigned b) { return a > b ? a : b; }
static int clamp(int n, int a, int b) { return n < a ? a : n > b ? b : n; }
static Snapshot *snapshot(void) { return &ARENA->history[(state.first + state.current) % HISTORY]; }
static SheetDoc *document(void) { return &snapshot()->doc; }
static void selection(unsigned *r0, unsigned *c0, unsigned *r1, unsigned *c1) {
    unsigned a = snapshot()->anchor, b = snapshot()->caret;
    *r0 = min_u(a / SHEET_COLS, b / SHEET_COLS); *r1 = max_u(a / SHEET_COLS, b / SHEET_COLS);
    *c0 = min_u(a % SHEET_COLS, b % SHEET_COLS); *c1 = max_u(a % SHEET_COLS, b % SHEET_COLS);
}
static unsigned column_width(unsigned col) {
    unsigned width = SHEET_COLUMN_WIDTH_DEFAULT;
    (void)sheet_get_column_width(document(), col, &width);
    return width;
}
/* Every horizontal operation uses the same document-pixel coordinate system.
 * scroll_x may split either edge column, including a 320px column at 360px
 * minimum width. Never infer a column from a fixed-size division. */
static unsigned column_start(unsigned col) {
    unsigned x = 0;
    for (unsigned c = 0; c < col; c++) x += column_width(c);
    return x;
}
static unsigned column_at(unsigned x) {
    unsigned c = 0;
    while (c + 1 < SHEET_COLS) {
        unsigned width = column_width(c);
        if (x < width) break;
        x -= width; c++;
    }
    return c;
}
static unsigned grid_width(void) {
    return fr.right > fr.row_w ? (unsigned)(fr.right - fr.row_w) : 1u;
}
static unsigned max_scroll_x(void) {
    unsigned total = column_start(SHEET_COLS), width = grid_width();
    return total > width ? total - width : 0;
}
static void horizontal_scroll(unsigned x) {
    state.scroll_x = min_u(x, max_scroll_x());
    state.first_col = column_at(state.scroll_x);
}
static void scroll_column(int direction) {
    unsigned col = column_at(state.scroll_x), start = column_start(col);
    if (direction < 0) horizontal_scroll(state.scroll_x > start ? start : col ? column_start(col - 1) : 0);
    else horizontal_scroll(column_start(col + 1));
}
static int column_left(unsigned col) { return fr.row_w + (int)column_start(col) - (int)state.scroll_x; }
static void geometry(int w, int h) {
    state.width = w; state.height = h; frame_layout(w, h);
    int rows = (fr.bottom - fr.grid_y) / ROW_H;
    state.visible_rows = min_u(rows > 1 ? (unsigned)rows : 1u, SHEET_ROWS);
    horizontal_scroll(state.scroll_x);
    state.first_row = min_u(state.first_row, SHEET_ROWS - state.visible_rows);
}
static void reveal(void) {
    unsigned row = snapshot()->caret / SHEET_COLS, col = snapshot()->caret % SHEET_COLS;
    unsigned left = column_start(col), right = left + column_width(col), width = grid_width();
    if (row < state.first_row) state.first_row = row;
    if (row >= state.first_row + state.visible_rows) state.first_row = row - state.visible_rows + 1;
    if (left < state.scroll_x || right - left > width) horizontal_scroll(left);
    else if (right > state.scroll_x + width) horizontal_scroll(right - width);
    else horizontal_scroll(state.scroll_x);
    state.blink = 1;
}
static void move_to(unsigned row, unsigned col, int extend) {
    snapshot()->caret = min_u(row, SHEET_ROWS - 1u) * SHEET_COLS + min_u(col, SHEET_COLS - 1u);
    if (!extend) snapshot()->anchor = snapshot()->caret;
    state.click_valid = 0; reveal();
}
static int allocate_revision(DocumentRevision *revision) {
    if (document_revision_next(&state.next_revision, revision)) return 1;
    status("Revision capacity exhausted. Current work is retained."); return 0;
}
/* Metadata controls may commit an edit and then add a second history state.
 * Preflight both identities so exhaustion cannot half-apply that action. */
static int revisions_available(unsigned count) {
    DocumentRevision last = state.next_revision, revision;
    while (count--) if (!document_revision_next(&last, &revision)) {
        status("Revision capacity exhausted. Current work is retained."); return 0;
    }
    return 1;
}
static int accept_staging(void) {
    DocumentRevision revision;
    if (!allocate_revision(&revision)) return 0;
    unsigned old = (state.first + state.current) % HISTORY;
    state.count = state.current + 1;
    if (state.count == HISTORY) { state.first = (state.first + 1) % HISTORY; state.current--; state.count--; }
    unsigned caret = ARENA->history[old].caret, anchor = ARENA->history[old].anchor;
    state.current++; state.count++;
    Snapshot *s = snapshot(); copy_bytes(&s->doc, &ARENA->staging, sizeof(SheetDoc));
    s->caret = caret; s->anchor = anchor; s->revision = revision;
    status("Edited. Ctrl+S saves .bsh; Ctrl+Shift+E exports calculated CSV values.");
    return 1;
}
static int same_source(const SheetCell *a, const SheetCell *b) {
    if (a->kind != b->kind || a->length != b->length) return 0;
    for (unsigned i = 0; i < a->length; i++) if (a->text[i] != b->text[i]) return 0;
    return 1;
}
static void start_edit(int replace) {
    const SheetCell *c = &document()->cells[snapshot()->caret];
    state.mode = 1; state.edit_changed = 0;
    state.edit_kind = !replace && c->kind == SHEET_TEXT ? SHEET_TEXT : SHEET_AUTO;
    state.edit_length = replace ? 0 : c->length;
    copy_bytes(state.edit, c->text, state.edit_length); state.edit[state.edit_length] = 0;
    state.edit_caret = state.edit_anchor = state.edit_length; state.blink = 1;
    status(state.edit_kind == SHEET_TEXT ? "Editing literal text (kind retained). Enter commits; Esc cancels. 95 bytes max." :
           "Edit: Enter/Tab commits; Esc cancels. 95 bytes; numbers -2147483.648 to 2147483.647.");
}
static int commit_edit(void) {
    if (state.mode != 1) return 1;
    if (state.edit_changed) {
        copy_bytes(&ARENA->staging, document(), sizeof(SheetDoc));
        unsigned at = snapshot()->caret;
        if (sheet_set(&ARENA->staging, at / SHEET_COLS, at % SHEET_COLS, state.edit_kind, state.edit, state.edit_length)) {
            status("Cell rejected. Use at most 95 ASCII bytes; Esc cancels the edit."); return 0;
        }
        if (!same_source(&ARENA->staging.cells[at], &document()->cells[at])) {
            if (sheet_recalculate(&ARENA->staging)) { status("Cannot calculate this document; edit retained."); return 0; }
            if (!accept_staging()) return 0;
        }
    }
    state.mode = state.edit_changed = 0; return 1;
}
static void cancel_edit(void) { state.mode = state.edit_changed = 0; status("Canceled. Cell is unchanged."); }
static int clear_selection(void) {
    unsigned r0, c0, r1, c1; selection(&r0, &c0, &r1, &c1); int changed = 0;
    copy_bytes(&ARENA->staging, document(), sizeof(SheetDoc));
    for (unsigned r = r0; r <= r1; r++) for (unsigned c = c0; c <= c1; c++) {
        if (sheet_cell(&ARENA->staging, r, c)->kind != SHEET_EMPTY) changed = 1;
        sheet_set(&ARENA->staging, r, c, SHEET_EMPTY, "", 0);
    }
    if (changed) { sheet_recalculate(&ARENA->staging); accept_staging(); }
    else status("Selected cells are already empty.");
    return SPREADSHEET_CHANGED;
}
static int undo(int redo) {
    if (state.mode) { cancel_edit(); return SPREADSHEET_CHANGED; }
    if ((!redo && !state.current) || (redo && state.current + 1 >= state.count)) {
        status(redo ? "Nothing to redo." : "Nothing to undo."); return SPREADSHEET_CHANGED;
    }
    state.current += redo ? 1 : (unsigned)-1; geometry(state.width, state.height); reveal(); status(redo ? "Redone." : "Undone.");
    return SPREADSHEET_CHANGED;
}
static int format_selection(SheetFormat format) {
    if (!revisions_available(state.mode == 1 && state.edit_changed ? 2u : 1u)) return SPREADSHEET_CHANGED;
    if (!commit_edit()) return SPREADSHEET_CHANGED;
    state.mode = 0; state.click_valid = 0;
    unsigned r0, c0, r1, c1; selection(&r0, &c0, &r1, &c1); int changed = 0;
    copy_bytes(&ARENA->staging, document(), sizeof(SheetDoc));
    for (unsigned r = r0; r <= r1; r++) {
        for (unsigned c = c0; c <= c1; c++) {
            SheetFormat old; (void)sheet_get_format(document(), r, c, &old);
            if (old != format) { sheet_set_format(&ARENA->staging, r, c, format); changed = 1; }
        }
        platform_poll();
    }
    if (changed && !accept_staging()) return SPREADSHEET_CHANGED;
    static const char *messages[] = {
        "General format. Original source and calculation precision are retained.",
        "Fixed2: two decimal places. Display rounding does not change the stored value.",
        "Currency: dollars, two decimal places. Display rounding does not change the value.",
        "Percent: value times 100, two decimal places. Stored value is unchanged."
    };
    status(messages[format]); return SPREADSHEET_CHANGED;
}
static int resize_columns(int delta) {
    if (!revisions_available(state.mode == 1 && state.edit_changed ? 2u : 1u)) return SPREADSHEET_CHANGED;
    if (!commit_edit()) return SPREADSHEET_CHANGED;
    state.mode = 0; state.click_valid = 0;
    unsigned r0, c0, r1, c1; selection(&r0, &c0, &r1, &c1); int changed = 0;
    copy_bytes(&ARENA->staging, document(), sizeof(SheetDoc));
    for (unsigned c = c0; c <= c1; c++) {
        unsigned old = column_width(c);
        unsigned width = delta ? (unsigned)clamp((int)old + delta, SHEET_COLUMN_WIDTH_MIN, SHEET_COLUMN_WIDTH_MAX) : SHEET_COLUMN_WIDTH_DEFAULT;
        if (old != width) { sheet_set_column_width(&ARENA->staging, c, width); changed = 1; }
    }
    if (changed) { if (!accept_staging()) return SPREADSHEET_CHANGED; geometry(state.width, state.height); reveal(); }
    status(delta ? "Selected columns resized. Width 48-320px; Ctrl+- / Ctrl+= adjusts; Ctrl+0 resets." :
                   "Selected column widths reset to 104px.");
    return SPREADSHEET_CHANGED;
}

/* Private clipboard: kind,length,format,source for each cell in row-major order.
 * External representation is quoted TSV of displayed values. Measure both
 * representations before touching either clipboard, so no truncation occurs. */
static unsigned field_size(const char *text, unsigned length) {
    unsigned size = length, quote = 0;
    for (unsigned i = 0; i < length; i++) { if (text[i] == '"') size++; if (text[i] == '"' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n') quote = 1; }
    return size + (quote ? 2u : 0u);
}
static unsigned emit_field(unsigned char *out, const char *text, unsigned length) {
    int quote = field_size(text, length) != length; unsigned used = 0;
    if (quote) out[used++] = '"';
    for (unsigned i = 0; i < length; i++) { out[used++] = (unsigned char)text[i]; if (text[i] == '"') out[used++] = '"'; }
    if (quote) out[used++] = '"';
    return used;
}
static int copy_selection(void) {
    unsigned r0, c0, r1, c1; selection(&r0, &c0, &r1, &c1);
    unsigned raw = 0, plain = 0; char value[SHEET_TEXT_MAX + 1u]; unsigned length;
    for (unsigned r = r0; r <= r1; r++) {
        for (unsigned c = c0; c <= c1; c++) {
            const SheetCell *cell = sheet_cell(document(), r, c);
            if (sheet_format_display(document(), r, c, value, sizeof value, &length)) return 0;
            raw += 3u + cell->length; plain += field_size(value, length) + (c < c1 ? 1u : 0u);
        }
        if (r < r1) plain++;
        platform_poll();
    }
    if (raw > SPREADSHEET_CLIPBOARD_CAPACITY || plain >= SPREADSHEET_CLIPBOARD_CAPACITY) {
        status("Copy exceeds the 64 KiB clipboard bound. Select a smaller range; nothing was cut."); return 0;
    }
    unsigned b = 0;
    for (unsigned r = r0; r <= r1; r++) {
        for (unsigned c = c0; c <= c1; c++) {
            sheet_format_display(document(), r, c, value, sizeof value, &length); b += emit_field(ARENA->output + b, value, length);
            if (c < c1) ARENA->output[b++] = '\t';
        }
        if (r < r1) ARENA->output[b++] = '\n';
        platform_poll();
    }
    unsigned generation = spreadsheet_clipboard_set((const char *)ARENA->output, plain);
    if (!generation) { status("Clipboard rejected the complete copy. Nothing was cut or replaced."); return 0; }
    unsigned a = 0;
    for (unsigned r = r0; r <= r1; r++) {
        for (unsigned c = c0; c <= c1; c++) {
            const SheetCell *cell = sheet_cell(document(), r, c);
            ARENA->clipboard[a++] = cell->kind; ARENA->clipboard[a++] = cell->length;
            SheetFormat format; (void)sheet_get_format(document(), r, c, &format);
            ARENA->clipboard[a++] = (unsigned char)format;
            copy_bytes(ARENA->clipboard + a, cell->text, cell->length); a += cell->length;
        }
        platform_poll();
    }
    state.clipboard_length = raw; state.clipboard_text_length = plain;
    state.clipboard_rows = r1 - r0 + 1; state.clipboard_cols = c1 - c0 + 1;
    state.clipboard_generation = generation; state.clipboard_owned = 1;
    status("Copied range. Own pastes retain formulas exactly; relative references are not adjusted."); return 1;
}
static int numeric_text(const char *s, unsigned n) {
    unsigned i = 0, digits = 0, dot = 0;
    if (i < n && (s[i] == '+' || s[i] == '-')) i++;
    for (; i < n; i++) {
        if (s[i] >= '0' && s[i] <= '9') digits++;
        else if (s[i] == '.' && !dot) dot = 1;
        else return 0;
    }
    return digits != 0;
}
static int paste_tsv(const unsigned char *data, unsigned n, unsigned row, unsigned col,
                     unsigned *rows, unsigned *cols) {
    unsigned p = 0, r = 0, c = 0, width = 0;
    while (1) {
        char field[SHEET_TEXT_MAX + 1u]; unsigned used = 0; int quoted = p < n && data[p] == '"';
        if (quoted) p++;
        int closed = !quoted;
        while (p < n) {
            unsigned ch = data[p];
            if (quoted) {
                if (ch == '"') {
                    if (p + 1 < n && data[p + 1] == '"') p++;
                    else { p++; closed = 1; break; }
                }
            } else if (ch == '\t' || ch == '\n' || ch == '\r') break;
            else if (ch == '"') return 0;
            if (used >= SHEET_TEXT_MAX || (ch < 32 && ch != '\t' && ch != '\r' && ch != '\n') || ch > 126) return 0;
            field[used++] = (char)ch; p++;
        }
        if (!closed || row + r >= SHEET_ROWS || col + c >= SHEET_COLS) return 0;
        if (p < n && data[p] != '\t' && data[p] != '\r' && data[p] != '\n') return 0;
        SheetKind kind = !used ? SHEET_EMPTY : numeric_text(field, used) ? SHEET_NUMBER : SHEET_TEXT;
        if (sheet_set(&ARENA->staging, row + r, col + c, kind, field, used)) return 0;
        width = max_u(width, c + 1);
        if (p == n) break;
        unsigned ch = data[p++];
        if (ch == '\t') { c++; continue; }
        if (ch == '\r' && (p == n || data[p++] != '\n')) return 0;
        if (p == n) break;
        r++; c = 0;
    }
    *rows = r + 1; *cols = width; return 1;
}
static void paste(void) {
    unsigned generation = 0;
    int n = spreadsheet_clipboard_get((char *)ARENA->output, SPREADSHEET_CLIPBOARD_CAPACITY, &generation);
    int own = state.clipboard_owned && ((n == -2 && generation && generation == state.clipboard_generation) ||
              (n >= 0 && generation == state.clipboard_generation && (unsigned)n == state.clipboard_text_length));
    if (!own && n <= 0) { status(n ? "Clipboard unavailable or larger than 64 KiB. Nothing was changed." : "Clipboard is empty."); return; }
    unsigned row = snapshot()->caret / SHEET_COLS, col = snapshot()->caret % SHEET_COLS, rows = 0, cols = 0;
    copy_bytes(&ARENA->staging, document(), sizeof(SheetDoc));
    if (own) {
        rows = state.clipboard_rows; cols = state.clipboard_cols;
        if (rows > SHEET_ROWS - row || cols > SHEET_COLS - col) {
            status("Paste would extend beyond Z128. Nothing was changed."); return;
        }
        unsigned p = 0;
        for (unsigned r = 0; r < rows; r++) for (unsigned c = 0; c < cols; c++) {
            unsigned kind = ARENA->clipboard[p++], length = ARENA->clipboard[p++], format = ARENA->clipboard[p++];
            if (sheet_set(&ARENA->staging, row + r, col + c, (SheetKind)kind, (const char *)ARENA->clipboard + p, length) ||
                sheet_set_format(&ARENA->staging, row + r, col + c, (SheetFormat)format)) {
                status("Clipboard invalid. Nothing was changed."); return;
            }
            p += length;
        }
    } else if ((unsigned)n >= SPREADSHEET_CLIPBOARD_CAPACITY || !paste_tsv(ARENA->output, (unsigned)n, row, col, &rows, &cols)) {
        status("Paste rejected: 95 ASCII bytes per cell, quoted TSV, and no cells beyond Z128."); return;
    }
    sheet_recalculate(&ARENA->staging); if (!accept_staging()) return;
    snapshot()->anchor = row * SHEET_COLS + col;
    snapshot()->caret = (row + rows - 1) * SHEET_COLS + col + cols - 1; reveal();
    status(own ? "Pasted exact sources, kinds and formats. Formula references were not adjusted." : "Pasted TSV values. Formula-like fields are literal text.");
}

static void reset_view(void) {
    state.first_row = state.first_col = state.scroll_x = 0; state.dragging = state.mode = state.edit_changed = state.click_valid = 0;
    geometry(state.width, state.height); state.blink = 1;
}
void spreadsheet_new(void) {
    if (!state.initialized) { spreadsheet_init(); return; }
    DocumentRevision revision;
    if (!allocate_revision(&revision)) return;
    document_save_detach(&state.save); state.export_retry.valid = 0;
    state.first = state.current = 0; state.count = 1;
    Snapshot *s = snapshot(); sheet_init(&s->doc); s->caret = s->anchor = 0;
    s->revision = revision; state.saved_revision = s->revision;
    state.file = -1; state.identity = state.incarnation = 0; state.failed_save = state.has_binding = state.binding_conflict = 0;
    reset_view(); text_copy(state.title, sizeof state.title, "Untitled sheet");
    status("A1:Z128 | 95 bytes/cell | numbers -2147483.648 to 2147483.647 | 3 decimal places.");
}
void spreadsheet_init(void) {
    if (state.initialized) return;
    state.initialized = 1; state.width = SPREADSHEET_W; state.height = SPREADSHEET_H;
    spreadsheet_new();
}
static int extension(const char *name, const char *suffix) {
    unsigned n = text_length(name);
    if (n < 4 || name[n - 4] != '.') return 0;
    for (unsigned i = 0; i < 3; i++) {
        unsigned c = (unsigned char)name[n - 3 + i];
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c != (unsigned char)suffix[i]) return 0;
    }
    return 1;
}
static int binding_valid(void) {
    return state.file >= 0 && fs_valid(state.file) && !fs_is_dir(state.file) && !fs_is_app(state.file) &&
           state.identity && state.incarnation && state.incarnation == fs_incarnation() &&
           fs_content_revision(state.file) && extension(fs_name(state.file), "bsh") &&
           fs_identity(state.file) == state.identity;
}
static void fingerprint(const unsigned char *data, unsigned size, SpreadsheetBinding *out) {
    unsigned fnv = 2166136261u, crc = ~0u;
    for (unsigned i = 0; i < size; i++) {
        fnv = (fnv ^ data[i]) * 16777619u; crc ^= data[i];
        for (unsigned bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        if ((i & 2047u) == 2047u) platform_poll();
    }
    out->size = size; out->hash_a = fnv; out->hash_b = ~crc;
}
int spreadsheet_binding_matches(int file, const SpreadsheetBinding *binding) {
    if (!binding || !fs_valid(file) || fs_is_dir(file) || fs_is_app(file) || !extension(fs_name(file), "bsh")) return 0;
    int size = fs_size(file); const unsigned char *data = (const unsigned char *)fs_data(file);
    if (size < (int)SHEET_NATIVE_HEADER_SIZE || (unsigned)size > SHEET_NATIVE_MAX_SIZE ||
        (unsigned)size != binding->size || !data) return 0;
    SpreadsheetBinding actual; fingerprint(data, (unsigned)size, &actual);
    return actual.hash_a == binding->hash_a && actual.hash_b == binding->hash_b;
}
int spreadsheet_binding(SpreadsheetBinding *out) {
    if (!out || !state.initialized || !state.has_binding || !binding_valid()) return 0;
    *out = state.binding; return 1;
}
int spreadsheet_open_file(int id) {
    if (!state.initialized) spreadsheet_init();
    if (!fs_valid(id) || fs_is_dir(id) || fs_is_app(id)) { status("That file is unavailable."); return 0; }
    int native = extension(fs_name(id), "bsh"), csv = extension(fs_name(id), "csv");
    if (!native && !csv) { status("Open a native .bsh or import a .csv file."); return 0; }
    int size = fs_size(id); const unsigned char *data = (const unsigned char *)fs_data(id);
    if (size < 0 || !data || (native ? sheet_native_decode(&ARENA->staging, data, (unsigned)size) :
                                             sheet_csv_import(&ARENA->staging, data, (unsigned)size))) {
        status(native ? "Invalid/unsupported .bsh. Current sheet and pending edit are unchanged." :
                        "CSV rejected. Use ASCII, 26x128 cells and at most 95 bytes per field."); return 0;
    }
    DocumentRevision revision;
    if (!allocate_revision(&revision)) return 0;
    document_save_detach(&state.save); state.export_retry.valid = 0;
    state.first = state.current = 0; state.count = 1;
    Snapshot *s = snapshot(); copy_bytes(&s->doc, &ARENA->staging, sizeof(SheetDoc));
    s->caret = s->anchor = 0; s->revision = revision;
    state.saved_revision = native ? s->revision : DOCUMENT_REVISION_NONE;
    state.file = native ? id : -1; state.identity = native ? fs_identity(id) : 0;
    state.incarnation = native ? fs_incarnation() : 0;
    state.has_binding = native; state.failed_save = state.binding_conflict = 0;
    if (native) fingerprint(data, (unsigned)size, &state.binding);
    reset_view(); text_copy(state.title, sizeof state.title, fs_name(id));
    status(native ? "Opened native sheet. F2 edits; typing replaces; Ctrl+G jumps to an A1 address." :
           "Imported CSV as an editable copy. Formula-like text is literal; save a new .bsh."); return 1;
}
const unsigned char *spreadsheet_snapshot(unsigned *length) {
    if (!state.initialized) spreadsheet_init();
    const SheetDoc *d = document();
    /* Autosave must never interrupt the editor. A candidate captures pending
     * bytes, while the current committed model and undo history remain intact. */
    if (state.mode == 1 && state.edit_changed) {
        copy_bytes(&ARENA->staging, d, sizeof(SheetDoc));
        unsigned at = snapshot()->caret;
        if (sheet_set(&ARENA->staging, at / SHEET_COLS, at % SHEET_COLS, state.edit_kind, state.edit, state.edit_length)) {
            status("Cannot snapshot pending cell edit; current work remains open."); return 0;
        }
        d = &ARENA->staging;
    }
    if (sheet_native_encode(d, ARENA->output, OUTPUT_CAPACITY, length)) { status("Could not encode this sheet."); return 0; }
    return ARENA->output;
}
static void apply_persistence_result(void) {
    DocumentSave *save = &state.save;
    if (save->kind == DOCUMENT_SAVE_NATIVE) {
        if (save->result == BOS_OK && (!binding_valid() || state.file != save->target.file ||
            state.identity != save->target.identity || !document_target_matches(&save->target)))
            save->result = BOS_E_STALE;
        if (save->result == BOS_OK) {
            state.saved_revision = save->revision; state.failed_save = 0;
            text_copy(state.title, sizeof state.title, fs_name(state.file));
            status(spreadsheet_dirty() ? "Saved submitted sheet. Newer edits remain unsaved." : "Saved native .bsh to disk.");
        } else {
            state.failed_save = 1; status(document_save_error(save));
        }
    } else if (save->kind == DOCUMENT_SAVE_CSV) {
        if (save->result == BOS_OK) {
            state.export_retry.valid = 0;
            status("CSV values exported. Formula-like text may execute in other spreadsheet programs.");
        } else status(save->result == BOS_E_IO ?
            "CSV is RAM-only. Retry CSV export with the same name." : document_save_error(save));
    }
}
int spreadsheet_persistence_poll(void) {
    if (!state.initialized || !document_save_poll(&state.save)) return 0;
    apply_persistence_result(); return SPREADSHEET_CHANGED;
}
void spreadsheet_save_info(DocumentSave *out) { if (out) *out = state.save; }
static int begin_persistence(unsigned kind, int id, unsigned length) {
    int result = document_save_begin(&state.save, kind, snapshot()->revision, id, length, 0);
    if (result == DOCUMENT_SAVE_PENDING) {
        status(kind == DOCUMENT_SAVE_NATIVE ? "Saving sheet to disk..." : "Exporting CSV to disk...");
    } else apply_persistence_result();
    return result;
}
static int persistence_active(void) {
    if (!state.save.pending) return 0;
    status("A save/export is pending. Retry after it finishes."); return 1;
}
static int storage_available(void) {
    if (!fs_sync_busy()) return 1;
    status("Disk is saving; retry shortly."); return 0;
}
static int version_available(int creating) {
    if (fs_version_available(creating)) return 1;
    status("File version capacity exhausted. Current work is retained."); return 0;
}
static int persistence_owner(void) {
    if (document_save_ensure_owner(&state.save)) return 1;
    status("Save owner capacity exhausted. Current work is retained."); return 0;
}
static int valid_parent_name(int parent, const char *name, const char *suffix) {
    if (!name || !extension(name, suffix) || text_length(name) >= FS_NAME_LEN) {
        status(suffix[0] == 'b' ? "Native sheets need a valid .bsh filename." : "CSV exports need a valid .csv filename."); return 0;
    }
    for (unsigned i = 0; name[i]; i++) if (name[i] == '/') {
        status("Choose a filename without path separators."); return 0;
    }
    if (!fs_valid(parent) || !fs_is_dir(parent) || !fs_identity(parent)) {
        status("Destination folder is unavailable. Current work is retained."); return 0;
    }
    return 1;
}
/* Only incomplete new placeholders may be removed. A complete zero-byte CSV
 * is still an installed output, irrespective of its later durability result. */
static void rollback_placeholder(int id, unsigned identity, unsigned incarnation) {
    if (identity && incarnation == fs_incarnation() && fs_valid(id) &&
        fs_identity(id) == identity && !fs_is_dir(id) && !fs_is_app(id) && fs_size(id) == 0)
        (void)fs_delete(id);
}
static int save_to(int id, unsigned length, int *installed) {
    *installed = 0;
    if (!version_available(0)) return SPREADSHEET_SAVE_ERROR;
    int written = fs_write(id, (const char *)ARENA->output, (int)length);
    if (written == FS_ERR_BUSY) { status("Disk is saving; retry shortly."); return SPREADSHEET_SAVE_ERROR; }
    if (written != (int)length) {
        state.failed_save = 1; status("Save failed. The complete sheet remains open."); return SPREADSHEET_SAVE_ERROR;
    }
    *installed = 1;
    state.file = id; state.identity = fs_identity(id); state.incarnation = fs_incarnation();
    state.has_binding = 1; state.binding_conflict = 0;
    fingerprint(ARENA->output, length, &state.binding); text_copy(state.title, sizeof state.title, fs_name(id));
    state.failed_save = 1;
    return begin_persistence(DOCUMENT_SAVE_NATIVE, id, length);
}
int spreadsheet_save(void) {
    if (!state.initialized) spreadsheet_init();
    if (persistence_active()) return state.save.kind == DOCUMENT_SAVE_NATIVE ? SPREADSHEET_SAVE_PENDING : SPREADSHEET_SAVE_ERROR;
    if (!storage_available()) return SPREADSHEET_SAVE_ERROR;
    if (!binding_valid()) { status("Choose Save As for a new native .bsh file."); return SPREADSHEET_SAVE_NEEDS_NAME; }
    if (!state.has_binding || !spreadsheet_binding_matches(state.file, &state.binding)) {
        state.binding_conflict = 1;
        status("Source changed outside Spreadsheet. Save As a new .bsh; source was not replaced.");
        return SPREADSHEET_SAVE_NEEDS_NAME;
    }
    if (!persistence_owner() || !version_available(0) || !commit_edit()) return SPREADSHEET_SAVE_ERROR;
    unsigned length;
    if (!spreadsheet_snapshot(&length)) return SPREADSHEET_SAVE_ERROR;
    int installed;
    return save_to(state.file, length, &installed);
}
int spreadsheet_save_as(int parent, const char *name) {
    if (!state.initialized) spreadsheet_init();
    if (persistence_active() || !storage_available()) return SPREADSHEET_SAVE_ERROR;
    if (!valid_parent_name(parent, name, "bsh")) return SPREADSHEET_SAVE_ERROR;
    int existing = fs_find_child(parent, name);
    if (existing >= 0) {
        if (binding_valid() && existing == state.file) return spreadsheet_save();
        status("Name already exists. Choose a new name; no file was replaced."); return SPREADSHEET_SAVE_ERROR;
    }
    if (!persistence_owner() || !version_available(1) || !commit_edit()) return SPREADSHEET_SAVE_ERROR;
    unsigned length;
    if (!spreadsheet_snapshot(&length)) return SPREADSHEET_SAVE_ERROR;
    if (length > fs_file_limit()) { status("Sheet exceeds this volume's file limit; no file was created."); return SPREADSHEET_SAVE_ERROR; }
    if (!version_available(1)) return SPREADSHEET_SAVE_ERROR;
    int id = fs_create(parent, name);
    if (id < 0) { status("Could not create file. Check filename, free space and disk status."); return SPREADSHEET_SAVE_ERROR; }
    unsigned identity = fs_identity(id), incarnation = fs_incarnation(); int installed;
    int result = save_to(id, length, &installed);
    if (!installed) rollback_placeholder(id, identity, incarnation);
    return result;
}
int spreadsheet_export_retry_name(char *out, unsigned capacity) {
    const SpreadsheetExportRetry *retry = &state.export_retry;
    if (!out || !retry->valid || !document_target_matches(&retry->target) ||
        !fs_valid(retry->parent) || !fs_is_dir(retry->parent) || !retry->parent_identity ||
        fs_identity(retry->parent) != retry->parent_identity ||
        fs_find_child(retry->parent, retry->name) != retry->target.file ||
        !document_revision_equal(retry->revision, snapshot()->revision) ||
        (state.mode == 1 && state.edit_changed) || text_length(retry->name) >= capacity) return 0;
    text_copy(out, capacity, retry->name); return 1;
}
static int export_retry_matches(int parent, const char *name, int id) {
    SpreadsheetExportRetry *retry = &state.export_retry;
    if (!retry->valid || retry->parent != parent || !retry->parent_identity ||
        fs_identity(parent) != retry->parent_identity || kstrcmp(retry->name, name) ||
        retry->target.file != id || !document_target_matches(&retry->target) ||
        !document_revision_equal(retry->revision, snapshot()->revision) ||
        (state.mode == 1 && state.edit_changed)) return 0;
    unsigned length;
    if (sheet_csv_export(document(), ARENA->output, OUTPUT_CAPACITY, &length) || length != retry->target.length) return 0;
    const unsigned char *data = (const unsigned char *)fs_data(id);
    if (length && !data) return 0;
    for (unsigned i = 0; i < length; i++) {
        if (data[i] != ARENA->output[i]) return 0;
        if ((i & 4095u) == 4095u) platform_poll();
    }
    return 1;
}
int spreadsheet_export_csv(int parent, const char *name) {
    if (!state.initialized) spreadsheet_init();
    if (persistence_active() || !storage_available()) return -1;
    if (!valid_parent_name(parent, name, "csv")) return -1;
    int existing = fs_find_child(parent, name);
    if (existing >= 0) {
        if (!export_retry_matches(parent, name, existing)) {
            status("Choose a new CSV name; existing files are never replaced."); return -1;
        }
        if (!persistence_owner()) return -1;
        int result = begin_persistence(DOCUMENT_SAVE_CSV, existing, state.export_retry.target.length);
        return result == DOCUMENT_SAVE_PENDING ? SPREADSHEET_EXPORT_PENDING : result == DOCUMENT_SAVE_OK ? existing : -1;
    }
    if (!persistence_owner() || !version_available(1) || !commit_edit()) return -1;
    unsigned length;
    if (sheet_csv_export(document(), ARENA->output, OUTPUT_CAPACITY, &length)) { status("CSV export failed; native sheet is unchanged."); return -1; }
    if (length > fs_file_limit()) { status("CSV exceeds this volume's file limit; no export was created."); return -1; }
    if (!version_available(1)) return -1;
    int id = fs_create(parent, name);
    if (id < 0) { status("Could not create CSV export."); return -1; }
    unsigned identity = fs_identity(id), incarnation = fs_incarnation();
    if (!version_available(0)) { rollback_placeholder(id, identity, incarnation); return -1; }
    if (fs_write(id, (const char *)ARENA->output, (int)length) != (int)length) {
        rollback_placeholder(id, identity, incarnation);
        status("CSV write failed. Native sheet remains open."); return -1;
    }
    SpreadsheetExportRetry retry = {0};
    retry.valid = document_target_capture(id, length, &retry.target);
    retry.parent = parent; retry.parent_identity = fs_identity(parent); retry.revision = snapshot()->revision;
    text_copy(retry.name, sizeof retry.name, name); state.export_retry = retry;
    int result = begin_persistence(DOCUMENT_SAVE_CSV, id, length);
    return result == DOCUMENT_SAVE_PENDING ? SPREADSHEET_EXPORT_PENDING : result == DOCUMENT_SAVE_OK ? id : -1;
}
int spreadsheet_restore(const unsigned char *data, unsigned length, int file, unsigned identity,
                        int dirty, unsigned caret, unsigned anchor) {
    if (!state.initialized) spreadsheet_init();
    if (sheet_native_decode(&ARENA->staging, data, length)) { status("Invalid recovery sheet. Current work is unchanged."); return 0; }
    DocumentRevision revision;
    if (!allocate_revision(&revision)) return 0;
    document_save_detach(&state.save); state.export_retry.valid = 0;
    state.first = state.current = 0; state.count = 1;
    Snapshot *s = snapshot(); copy_bytes(&s->doc, &ARENA->staging, sizeof(SheetDoc));
    s->caret = min_u(caret, SHEET_CELLS - 1); s->anchor = min_u(anchor, SHEET_CELLS - 1);
    s->revision = revision;
    state.file = file; state.identity = identity; state.incarnation = fs_incarnation();
    state.has_binding = state.binding_conflict = state.failed_save = 0;
    int size = binding_valid() ? fs_size(file) : -1;
    const unsigned char *source = size >= 0 ? (const unsigned char *)fs_data(file) : 0;
    if (size < (int)SHEET_NATIVE_HEADER_SIZE || (unsigned)size > SHEET_NATIVE_MAX_SIZE || !source) {
        state.file = -1; state.identity = state.incarnation = 0; dirty = 1;
    } else { fingerprint(source, (unsigned)size, &state.binding); state.has_binding = 1; }
    state.saved_revision = dirty ? DOCUMENT_REVISION_NONE : s->revision;
    text_copy(state.title, sizeof state.title, state.file >= 0 ? fs_name(state.file) : "Recovered sheet");
    reset_view(); reveal(); status("Recovered sheet draft."); return 1;
}
const char *spreadsheet_title(void) { return state.initialized ? state.title : "Untitled sheet"; }
const char *spreadsheet_status(void) { return state.status; }
int spreadsheet_dirty(void) {
    return state.initialized && (state.failed_save || state.binding_conflict || (state.mode == 1 && state.edit_changed) ||
           !document_revision_equal(snapshot()->revision, state.saved_revision) || (state.file >= 0 && !binding_valid()));
}
int spreadsheet_file(void) { return state.initialized && binding_valid() ? state.file : -1; }
unsigned spreadsheet_file_identity(void) { return state.initialized && binding_valid() ? state.identity : 0; }
unsigned spreadsheet_caret(void) { return state.initialized ? snapshot()->caret : 0; }
unsigned spreadsheet_anchor(void) { return state.initialized ? snapshot()->anchor : 0; }
const SheetDoc *spreadsheet_document(void) { if (!state.initialized) spreadsheet_init(); return document(); }
int spreadsheet_editing(void) { return state.mode == 1; }
unsigned spreadsheet_first_row(void) { return state.first_row; }
unsigned spreadsheet_first_col(void) { return state.first_col; }
void spreadsheet_close(void) {
    if (state.initialized) spreadsheet_new();
    /* A closed window never retains interest, even if revision exhaustion
     * prevented replacing its private model with a fresh blank sheet. */
    document_save_detach(&state.save); state.export_retry.valid = 0;
}
void spreadsheet_release(void) { state.dragging = 0; }
int spreadsheet_scroll(int lines) {
    if (!state.initialized) spreadsheet_init();
    unsigned old = state.first_row;
    state.first_row = (unsigned)clamp((int)state.first_row + clamp(lines, -128, 128), 0, (int)(SHEET_ROWS - state.visible_rows));
    return old != state.first_row;
}
int spreadsheet_tick(void) {
    if (!state.initialized || !state.mode) return 0;
    int blink = (int)((timer_ticks() / 35u) & 1u);
    if (blink != state.last_blink) { state.last_blink = blink; state.blink = !blink; return SPREADSHEET_CHANGED; }
    return 0;
}

static void open_address(void) {
    if (!commit_edit()) return;
    state.mode = 2; sheet_label(snapshot()->caret / SHEET_COLS, snapshot()->caret % SHEET_COLS, state.edit, sizeof state.edit);
    state.edit_length = text_length(state.edit); state.edit_caret = state.edit_length; state.edit_anchor = 0;
    state.blink = 1; status("Go to A1 through Z128. Enter jumps; Esc cancels.");
}
static int edit_replace(const char *text, unsigned length) {
    unsigned lo = min_u(state.edit_caret, state.edit_anchor), hi = max_u(state.edit_caret, state.edit_anchor);
    unsigned max = state.mode == 2 ? 4u : SHEET_TEXT_MAX;
    if (length > max - (state.edit_length - (hi - lo))) {
        status(state.mode == 2 ? "Address is at most four bytes, A1 through Z128." : "Cell limit is 95 bytes. Nothing was truncated."); return 0;
    }
    for (unsigned i = 0; i < length; i++) {
        unsigned c = (unsigned char)text[i];
        if ((c < 32 && c != '\t' && c != '\r' && c != '\n') || c > 126) {
            status("Only printable ASCII, tabs and line breaks are supported."); return 0;
        }
    }
    unsigned end = state.edit_length - (hi - lo) + length;
    if (lo + length > hi) {
        for (unsigned i = state.edit_length; i > hi; i--) state.edit[i - hi + lo + length - 1] = state.edit[i - 1];
    } else for (unsigned i = hi; i < state.edit_length; i++) state.edit[i - hi + lo + length] = state.edit[i];
    for (unsigned i = 0; i < length; i++) state.edit[lo + i] = text[i];
    state.edit_length = end; state.edit[end] = 0; state.edit_caret = state.edit_anchor = lo + length;
    if (state.mode == 1 && (length || hi != lo)) state.edit_changed = 1;
    state.blink = 1; return 1;
}
static void edit_copy(int cut) {
    unsigned lo = min_u(state.edit_caret, state.edit_anchor), hi = max_u(state.edit_caret, state.edit_anchor);
    if (hi == lo) { status("Select text in the formula bar to copy."); return; }
    unsigned generation = spreadsheet_clipboard_set(state.edit + lo, hi - lo);
    if (!generation) { status("Clipboard rejected the copy. Selected edit text was not cut."); return; }
    ARENA->clipboard[0] = SHEET_TEXT; ARENA->clipboard[1] = (unsigned char)(hi - lo);
    ARENA->clipboard[2] = SHEET_FORMAT_GENERAL;
    copy_bytes(ARENA->clipboard + 3, state.edit + lo, hi - lo);
    state.clipboard_length = hi - lo + 3; state.clipboard_rows = state.clipboard_cols = 1;
    state.clipboard_text_length = hi - lo; state.clipboard_owned = 1;
    state.clipboard_generation = generation;
    if (cut) edit_replace("", 0);
    status(cut ? "Cut selected formula-bar text." : "Copied selected formula-bar text.");
}
static void edit_paste(void) {
    unsigned generation = 0;
    int n = spreadsheet_clipboard_get((char *)ARENA->output, SPREADSHEET_CLIPBOARD_CAPACITY, &generation);
    const char *text = (const char *)ARENA->output;
    if (n == -2 && state.clipboard_owned && generation && generation == state.clipboard_generation && state.clipboard_rows == 1 && state.clipboard_cols == 1) {
        n = ARENA->clipboard[1]; text = (const char *)ARENA->clipboard + 3;
    }
    if (n < 0) { status("Clipboard unavailable or too large. Edit is unchanged."); return; }
    if (!n) { status("Clipboard is empty."); return; }
    edit_replace(text, (unsigned)n);
}
static int edit_key(int sc, char ch, int modifiers) {
    int ctrl = modifiers & SPREADSHEET_MOD_CTRL, shift = modifiers & SPREADSHEET_MOD_SHIFT;
    if (sc == 0x01) { cancel_edit(); return SPREADSHEET_CHANGED; }
    if (sc == 0x1c || sc == 0x0f) {
        if (state.mode == 2) {
            unsigned r, c;
            if (sheet_reference(state.edit, state.edit_length, &r, &c)) { status("Invalid address. Use A1 through Z128, without spaces or leading zeroes."); return SPREADSHEET_CHANGED; }
            state.mode = 0; move_to(r, c, 0); status("Jumped to cell."); return SPREADSHEET_CHANGED;
        }
        if (commit_edit()) {
            int row = (int)(snapshot()->caret / SHEET_COLS), col = (int)(snapshot()->caret % SHEET_COLS);
            if (sc == 0x1c) row += shift ? -1 : 1; else col += shift ? -1 : 1;
            move_to((unsigned)clamp(row, 0, SHEET_ROWS - 1), (unsigned)clamp(col, 0, SHEET_COLS - 1), 0);
        }
        return SPREADSHEET_CHANGED;
    }
    if (ctrl) {
        if (sc == 0x1e) { state.edit_anchor = 0; state.edit_caret = state.edit_length; return SPREADSHEET_CHANGED; }
        if (sc == 0x2e || sc == 0x2d) { edit_copy(sc == 0x2d); return SPREADSHEET_CHANGED; }
        if (sc == 0x2f) { edit_paste(); return SPREADSHEET_CHANGED; }
    }
    unsigned pos = state.edit_caret, lo = min_u(pos, state.edit_anchor), hi = max_u(pos, state.edit_anchor);
    if (sc == 0x4b || sc == 0x4d || sc == 0x47 || sc == 0x4f) {
        if (sc == 0x47) pos = 0;
        else if (sc == 0x4f) pos = state.edit_length;
        else if (!shift && lo != hi) pos = sc == 0x4b ? lo : hi;
        else if (sc == 0x4b) { if (pos) pos--; if (ctrl) pos = 0; }
        else { if (pos < state.edit_length) pos++; if (ctrl) pos = state.edit_length; }
        state.edit_caret = pos; if (!shift) state.edit_anchor = pos; state.blink = 1; return SPREADSHEET_CHANGED;
    }
    if (ctrl) return 0;
    if (sc == 0x0e || sc == 0x53) {
        if (lo == hi) {
            if (sc == 0x0e && lo) state.edit_anchor = lo - 1;
            else if (sc == 0x53 && hi < state.edit_length) state.edit_anchor = hi + 1;
        }
        edit_replace("", 0); return SPREADSHEET_CHANGED;
    }
    if ((unsigned char)ch >= 32 && (unsigned char)ch <= 126) { edit_replace(&ch, 1); return SPREADSHEET_CHANGED; }
    return 0;
}
int spreadsheet_key(int sc, char ch, int modifiers) {
    if (!state.initialized) spreadsheet_init();
    int ctrl = modifiers & SPREADSHEET_MOD_CTRL, shift = modifiers & SPREADSHEET_MOD_SHIFT;
    if (modifiers & SPREADSHEET_MOD_ALT) return 0;
    if (ctrl && sc == 0x1f) return shift ? SPREADSHEET_REQUEST_SAVE_AS : SPREADSHEET_REQUEST_SAVE;
    if (ctrl && shift && sc == 0x12) return SPREADSHEET_REQUEST_EXPORT;
    if (ctrl && sc == 0x22) { open_address(); return SPREADSHEET_CHANGED; }
    if (ctrl && (sc == 0x2c || sc == 0x15)) return undo(sc == 0x15 || shift);
    if (ctrl && sc >= 0x02 && sc <= 0x05) return format_selection((SheetFormat)(sc - 0x02));
    if (ctrl && (sc == 0x0b || sc == 0x0c || sc == 0x0d)) return resize_columns(sc == 0x0b ? 0 : sc == 0x0c ? -16 : 16);
    if (state.mode) return edit_key(sc, ch, modifiers);
    if (ctrl) {
        switch (sc) {
        case 0x1e: snapshot()->anchor = 0; snapshot()->caret = SHEET_CELLS - 1; reveal(); return SPREADSHEET_CHANGED;
        case 0x2e: copy_selection(); return SPREADSHEET_CHANGED;
        case 0x2d: if (copy_selection()) clear_selection(); return SPREADSHEET_CHANGED;
        case 0x2f: paste(); return SPREADSHEET_CHANGED;
        }
    }
    int row = (int)(snapshot()->caret / SHEET_COLS), col = (int)(snapshot()->caret % SHEET_COLS), moving = 1;
    switch (sc) {
    case 0x4b: col = ctrl ? 0 : col - 1; break;
    case 0x4d: col = ctrl ? (int)SHEET_COLS - 1 : col + 1; break;
    case 0x48: row = ctrl ? 0 : row - 1; break;
    case 0x50: row = ctrl ? (int)SHEET_ROWS - 1 : row + 1; break;
    case 0x47: col = 0; if (ctrl) row = 0; break;
    case 0x4f: col = SHEET_COLS - 1; if (ctrl) row = SHEET_ROWS - 1; break;
    case 0x49: row -= (int)state.visible_rows; break;
    case 0x51: row += (int)state.visible_rows; break;
    case 0x1c: row += shift ? -1 : 1; shift = 0; break;
    case 0x0f: col += shift ? -1 : 1; shift = 0; break;
    default: moving = 0;
    }
    if (moving) {
        move_to((unsigned)clamp(row, 0, SHEET_ROWS - 1), (unsigned)clamp(col, 0, SHEET_COLS - 1), shift);
        return SPREADSHEET_CHANGED;
    }
    if (ctrl) return 0;
    if (sc == 0x3c) { start_edit(0); return SPREADSHEET_CHANGED; }
    if (sc == 0x53 || sc == 0x0e) return clear_selection();
    if ((unsigned char)ch >= 32 && (unsigned char)ch <= 126) {
        start_edit(1); edit_replace(&ch, 1); return SPREADSHEET_CHANGED;
    }
    if (sc == 0x01) { snapshot()->anchor = snapshot()->caret; return SPREADSHEET_CHANGED; }
    return 0;
}


/* The graphics library clips to screen, not application windows. Every
 * primitive and antialiased glyph below is intersected with the client/cell;
 * kit controls paint unclipped, so each is shown and hit only when wholly
 * inside the client. */
static Clip intersect(Clip a, Clip b) {
    int right = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w;
    int bottom = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
    a.x = a.x > b.x ? a.x : b.x; a.y = a.y > b.y ? a.y : b.y;
    a.w = right > a.x ? right - a.x : 0; a.h = bottom > a.y ? bottom - a.y : 0; return a;
}
static void rect(Clip c, int x, int y, int w, int h, unsigned char color) {
    if (w <= 0 || h <= 0) return;
    Clip part = intersect(c, (Clip){x, y, w, h});
    if (part.w && part.h) draw_rect(part.x, part.y, part.w, part.h, color);
}
static void frame(Clip c, int x, int y, int w, int h, unsigned char color) {
    if (w <= 0 || h <= 0) return;
    rect(c, x, y, w, 1, color); rect(c, x, y + h - 1, w, 1, color);
    rect(c, x, y, 1, h, color); rect(c, x + w - 1, y, 1, h, color);
}
static int box_visible(Clip b, int w, int h) { return b.x >= 0 && b.y >= 0 && b.x + b.w <= w && b.y + b.h <= h; }
static char visible_char(char c) { return c == '\t' || c == '\n' || c == '\r' ? '?' : c; }
static int text_width(const char *text, unsigned length) {
    int w = 0; for (unsigned i = 0; i < length; i++) w += ui_advance(visible_char(text[i])); return w;
}
static void glyph(Clip c, char ch, int x, int y, unsigned char color) {
    ch = visible_char(ch);
    int bpr, h, w; const unsigned char *bits = ui_glyph(ch, &bpr, &h, &w);
    if (!bits || ch == ' ') return;
    c = intersect(c, (Clip){0, 0, fb_w, fb_h});
    for (int yy = 0; yy < h; yy++) {
        int py = y + yy; if (py < c.y || py >= c.y + c.h) continue;
        for (int xx = 0; xx < w; xx++) {
            int px = x + xx; if (px < c.x || px >= c.x + c.w) continue;
            int level = glyph_level(bits, bpr, yy, xx);
            if (level) put_pixel(px, py, level >= 15 ? color : gfx_mix(get_pixel(px, py), color, (level * 256 + 7) / 15));
        }
    }
}
static void label_n(Clip c, const char *text, unsigned length, int x, int y, unsigned char color) {
    for (unsigned i = 0; i < length; i++) {
        glyph(c, text[i], x, y, color); x += ui_advance(visible_char(text[i])); if (x >= c.x + c.w) break;
    }
}
static void label(Clip c, const char *text, int x, int y, unsigned char color) { label_n(c, text, text_length(text), x, y, color); }
static void number_text(unsigned n, char *out) {
    char reverse[10]; unsigned used = 0, i = 0;
    do { reverse[used++] = (char)('0' + n % 10u); n /= 10u; } while (n);
    while (used) out[i++] = reverse[--used];
    out[i] = 0;
}

/* The desktop overrides this weak default to report pointer hover and press
 * for a control rectangle (absolute coordinates) and to repaint on change. */
__attribute__((weak)) int spreadsheet_hover(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; return 0; }
void spreadsheet_set_active(int active) { state.inactive = !active; }
static int hot(int x, int y, int w, int h) { return state.inactive ? 0 : spreadsheet_hover(x, y, w, h); }

/* Style metrics. Classic: square controls on TK_BASE strips with subtle
 * rules; Modern: rounded controls, and the toolbar continues the header. */
static int modern(void) { return ui_theme.style == UI_STYLE_MODERN; }
static int ctl(void) { return ui_theme.control_h; }
static int tool_row_h(void) { return modern() ? 44 : 34; }
static int tool_row_gap(void) { return modern() ? 6 : 4; }
static int tool_margin(void) { return modern() ? 12 : 8; }
static int field_inset(void) { return modern() ? 11 : 7; }   /* kit_field text x */

/* Toolbar button: flat until hovered; toggles show their on state. Icons are
 * ICON_* toolbar glyphs, which the kit's icon button does not draw. */
static void tool_draw(int x, int y, int w, int h, int icon, const char *text, int state) {
    int r = modern() ? ui_theme.r_control : KIT_R_NOTCH, dis = state & KIT_DISABLED;
    uint8_t ink = TK_ICON, fore = TK_TEXT;
    if (state & KIT_ON) {
        if (modern()) { kit_fill(x, y, w, h, r, TK_ACCENT_100, 1); ink = fore = TK_ACCENT_700; }
        else kit_box(x, y, w, h, r, TK_RAISED, TK_LINE);
    } else if ((state & (KIT_HOVER | KIT_PRESSED)) && !dis) {
        kit_fill(x, y, w, h, r, TK_HOVER, 1);
        if ((state & KIT_PRESSED) && !modern()) kit_frame(x, y, w, h, r, TK_LINE);
    }
    if (dis) ink = fore = TK_LINE_STRONG;
    int content = (icon >= 0 ? ICON_SIZE : 0) + (text ? kit_text_w(text, 0) + (icon >= 0 ? 6 : 0) : 0);
    int cx = x + (w - content) / 2;
    if (icon >= 0) { icon_draw(icon, cx, y + (h - ICON_SIZE) / 2, ink); cx += ICON_SIZE + 6; }
    if (text) kit_text(text, cx, kit_ty(y, h), fore, x + w - cx - 2, 0);
}

/* Toolbar: one geometry routine feeds drawing and hit testing. Groups flow
 * left to right with dividers; a wide client fits one row, a narrow one
 * wraps the format/width groups onto a second row. */
enum { TOOL_SAVE, TOOL_SAVE_AS, TOOL_CSV, TOOL_UNDO, TOOL_REDO, TOOL_EDIT, TOOL_GO,
       TOOL_GENERAL, TOOL_FIXED2, TOOL_CURRENCY, TOOL_PERCENT, TOOL_NARROWER, TOOL_WIDER, TOOL_COUNT };
static const struct { signed char icon; unsigned char group; const char *label; } tool_items[TOOL_COUNT] = {
    {ICON_SAVE, 0, 0}, {ICON_SAVE_AS, 0, 0}, {ICON_SHEET, 0, "CSV"},
    {ICON_UNDO, 1, 0}, {ICON_REDO, 1, 0}, {ICON_EDIT, 2, 0}, {ICON_JUMP, 2, 0},
    {-1, 3, "General"}, {ICON_HASH, 3, 0}, {ICON_DOLLAR, 3, 0}, {ICON_PERCENT, 3, 0},
    {ICON_MINUS, 4, 0}, {ICON_PLUS, 4, 0}
};
#define TOOL_WRAP_GROUP 3
#define WIDTH_VALUE_W 36
typedef struct { int height, dividers; Clip item[TOOL_COUNT], divider[4], caption, value; } Toolbar;
static int tool_item_w(unsigned i) {
    const char *text = tool_items[i].label;
    if (tool_items[i].icon < 0) return ui_string_w(text) + 22;
    return text ? ICON_SIZE + 6 + ui_string_w(text) + 22 : ctl();
}
/* Returns the right edge of the laid-out controls. */
static int toolbar_flow(int wrap, Toolbar *t) {
    int s = ctl(), y0 = (tool_row_h() - 1 - s) / 2, ys[2] = {y0, y0 + s + tool_row_gap()};
    int row = 0, x = tool_margin(), right = 0, group = -1;
    t->height = tool_row_h() + (wrap ? s + tool_row_gap() : 0); t->dividers = 0;
    for (unsigned i = 0; i < TOOL_COUNT; i++) {
        int g = tool_items[i].group;
        if (g != group && group >= 0) {
            if (wrap && g == TOOL_WRAP_GROUP) { row = 1; x = tool_margin(); }
            else { t->divider[t->dividers++] = (Clip){x + 6, ys[row] + 5, 1, s - 10}; x += 13; }
        } else if (group >= 0) x += 4;
        group = g;
        if (i == TOOL_NARROWER) { t->caption = (Clip){x, ys[row], ui_string_w("Width"), s}; x += t->caption.w + 8; }
        if (i == TOOL_WIDER) { t->value = (Clip){x - 4, ys[row], WIDTH_VALUE_W, s}; x += WIDTH_VALUE_W - 4; }
        t->item[i] = (Clip){x, ys[row], tool_item_w(i), s};
        x += t->item[i].w; if (x > right) right = x;
    }
    return right;
}
static void toolbar_layout(int w, Toolbar *t) { if (toolbar_flow(0, t) + tool_margin() > w) toolbar_flow(1, t); }

static void frame_layout(int w, int h) {
    Toolbar bar; toolbar_layout(w, &bar);
    int s = ctl(), m = tool_margin();
    fr.tool_h = bar.height; fr.bar_y = fr.tool_h; fr.bar_h = s + (modern() ? 12 : 8);
    fr.head_y = fr.bar_y + fr.bar_h; fr.head_h = modern() ? 28 : 24; fr.grid_y = fr.head_y + fr.head_h;
    fr.sb = ui_theme.scrollbar; fr.status_h = modern() ? 28 : 24; fr.status_y = h - fr.status_h;
    fr.bottom = fr.status_y - fr.sb; fr.right = w - fr.sb; fr.row_w = modern() ? 46 : 42;
    int y = fr.bar_y + (fr.bar_h - s) / 2;
    /* Name box, Cancel and Enter (live while a cell is being edited), the
     * source kind, then the source field to the right edge. */
    fr.name = (Clip){m, y, 72, s};
    fr.cancel = (Clip){fr.name.x + fr.name.w + 6, y, s, s};
    fr.enter = (Clip){fr.cancel.x + s + 2, y, s, s};
    int source_x = fr.enter.x + s + 34;
    fr.source = (Clip){source_x, y, w - m - source_x, s};
}

static int editor_offset(int width) {
    int before = text_width(state.edit, state.edit_caret);
    return before >= width ? before - width + 8 : 0;
}
static void editor_draw(Clip all, Clip box) {
    box = intersect(all, box); if (!box.w || !box.h) return;
    unsigned lo = min_u(state.edit_caret, state.edit_anchor), hi = max_u(state.edit_caret, state.edit_anchor);
    int x = box.x - editor_offset(box.w);
    for (unsigned i = 0; i < state.edit_length; i++) {
        int selected = i >= lo && i < hi, width = ui_advance(visible_char(state.edit[i]));
        if (selected) rect(box, x, box.y, width, box.h, TK_ACCENT);
        glyph(box, state.edit[i], x, box.y, selected ? TK_ON_ACCENT : TK_TEXT); x += width;
    }
    if (state.blink && lo == hi && !state.inactive) {
        x = box.x - editor_offset(box.w) + text_width(state.edit, state.edit_caret);
        rect(box, x, box.y + 1, 1, 15, TK_TEXT);
    }
}
/* Text box inside a formula-bar field, window relative. */
static Clip field_text(Clip f) { return (Clip){f.x + field_inset(), f.y + (f.h - UI_FONT_H) / 2, f.w - field_inset() - 6, UI_FONT_H}; }
int spreadsheet_cell_position(unsigned row, unsigned col, int *x, int *y, int *w, int *h) {
    if (!state.initialized || row < state.first_row || col < state.first_col || row >= SHEET_ROWS || col >= SHEET_COLS) return 0;
    int left = column_left(col), top = fr.grid_y + (int)(row - state.first_row) * ROW_H;
    int right = left + (int)column_width(col), bottom = top + ROW_H;
    if (left < fr.row_w) left = fr.row_w;
    if (right > fr.right) right = fr.right;
    if (bottom > fr.bottom) bottom = fr.bottom;
    if (left >= right || top >= bottom) return 0;
    if (x) *x = left;
    if (y) *y = top;
    if (w) *w = right - left;
    if (h) *h = bottom - top;
    return 1;
}

/* Scrollbars: rows vertically, document pixels horizontally. The thumb
 * drags; a track click pages rows or steps one column. */
static unsigned total_width(void) { return column_start(SHEET_COLS); }
static void thumb(int vertical, int *at, int *size) {
    if (vertical) kit_scroll_thumb(fr.bottom - fr.grid_y - 4, (int)state.first_row, (int)state.visible_rows, SHEET_ROWS, at, size);
    else kit_scroll_thumb(fr.right - fr.row_w - 4, (int)state.scroll_x, (int)grid_width(), (int)total_width(), at, size);
}
static void draw_scrollbars(Clip all) {
    int x = all.x, y = all.y, sb = fr.sb, vlen = fr.bottom - fr.grid_y, hlen = fr.right - fr.row_w, at, size;
    uint8_t corner = modern() ? ui_theme.content : TK_BASE;
    rect(all, x + fr.right, y + fr.head_y, sb, fr.head_h, corner);
    rect(all, x, y + fr.bottom, fr.row_w, sb, corner);
    rect(all, x + fr.right, y + fr.bottom, sb, sb, corner);
    if (vlen >= 8 && box_visible((Clip){fr.right, fr.grid_y, sb, vlen}, all.w, all.h)) {
        thumb(1, &at, &size);
        int st = state.dragging && state.drag_kind == 2 && state.grab >= 0 ? KIT_PRESSED : hot(x + fr.right, y + fr.grid_y + 2 + at, sb, size);
        if (modern()) rect(all, x + fr.right, y + fr.grid_y, sb, vlen, ui_theme.content);
        kit_vscroll(x + fr.right, y + fr.grid_y, vlen, (int)state.first_row, (int)state.visible_rows, SHEET_ROWS, st);
    }
    if (hlen >= 8 && box_visible((Clip){fr.row_w, fr.bottom, hlen, sb}, all.w, all.h)) {
        thumb(0, &at, &size);
        int st = state.dragging && state.drag_kind == 3 && state.grab >= 0 ? KIT_PRESSED : hot(x + fr.row_w + 2 + at, y + fr.bottom, size, sb);
        if (modern()) rect(all, x + fr.row_w, y + fr.bottom, hlen, sb, ui_theme.content);
        kit_hscroll(x + fr.row_w, y + fr.bottom, hlen, (int)state.scroll_x, (int)grid_width(), (int)total_width(), st);
    }
}

static void draw_toolbar(Clip all, unsigned r0, unsigned c0, unsigned r1, unsigned c1) {
    int x = all.x, y = all.y, w = all.w, h = all.h;
    Toolbar bar; toolbar_layout(w, &bar);
    rect(all, x, y, w, bar.height - 1, modern() ? ui_theme.chrome : TK_BASE);
    rect(all, x, y + bar.height - 1, w, 1, modern() ? TK_LINE : TK_LINE_SUBTLE);
    int uniform_format = document()->formats[r0 * SHEET_COLS + c0];
    for (unsigned r = r0; r <= r1 && uniform_format >= 0; r++)
        for (unsigned c = c0; c <= c1; c++)
            if (document()->formats[r * SHEET_COLS + c] != uniform_format) { uniform_format = -1; break; }
    unsigned width = column_width(c0), narrowest = width, widest = width;
    for (unsigned c = c0 + 1; c <= c1; c++) { narrowest = min_u(narrowest, column_width(c)); widest = max_u(widest, column_width(c)); }
    for (int i = 0; i < bar.dividers; i++)
        if (box_visible(bar.divider[i], w, h)) kit_vseparator(x + bar.divider[i].x, y + bar.divider[i].y, bar.divider[i].h);
    for (unsigned i = 0; i < TOOL_COUNT; i++) {
        Clip b = bar.item[i]; if (!box_visible(b, w, h)) continue;
        /* Formats are a toggle group (on when uniform); the rest are actions.
         * Undo/Redo also cancel an open edit, so they stay live while editing. */
        int look = 0;
        if (i >= TOOL_GENERAL && i <= TOOL_PERCENT) look = (int)(i - TOOL_GENERAL) == uniform_format ? KIT_ON : 0;
        else if ((i == TOOL_UNDO && !state.mode && !state.current) ||
                 (i == TOOL_REDO && !state.mode && state.current + 1 >= state.count) ||
                 (i == TOOL_NARROWER && widest <= SHEET_COLUMN_WIDTH_MIN) ||
                 (i == TOOL_WIDER && narrowest >= SHEET_COLUMN_WIDTH_MAX)) look = KIT_DISABLED;
        if (!(look & KIT_DISABLED)) look |= hot(x + b.x, y + b.y, b.w, b.h);
        tool_draw(x + b.x, y + b.y, b.w, b.h, tool_items[i].icon, tool_items[i].label, look);
    }
    char width_text[11]; number_text(width, width_text);
    const char *shown = narrowest != widest ? "..." : width_text;
    if (box_visible(bar.caption, w, h))
        kit_text("Width", x + bar.caption.x, kit_ty(y + bar.caption.y, bar.caption.h), TK_TEXT2, bar.caption.w, 0);
    if (box_visible(bar.value, w, h))
        kit_text_center(shown, x + bar.value.x, kit_ty(y + bar.value.y, bar.value.h), bar.value.w, TK_TEXT, 0);
}

static void draw_formula_bar(Clip all) {
    int x = all.x, y = all.y, w = all.w, h = all.h, s = ctl();
    rect(all, x, y + fr.bar_y, w, fr.bar_h - 1, modern() ? ui_theme.content : TK_BASE);
    rect(all, x, y + fr.bar_y + fr.bar_h - 1, w, 1, TK_LINE_SUBTLE);
    Clip ring = {fr.name.x - 2, fr.name.y - 2, fr.name.w + 4, fr.name.h + 4};
    if (box_visible(ring, w, h)) {
        kit_field(x + fr.name.x, y + fr.name.y, fr.name.w, fr.name.h, state.mode == 2 && !state.inactive ? KIT_FOCUS : 0);
        Clip box = field_text(fr.name); box.x += x; box.y += y;
        if (state.mode == 2) editor_draw(all, box);
        else {
            char address[5]; sheet_label(snapshot()->caret / SHEET_COLS, snapshot()->caret % SHEET_COLS, address, sizeof address);
            label(intersect(all, box), address, box.x, box.y, TK_TEXT);
        }
    }
    if (box_visible(fr.enter, w, h)) {
        int live = state.mode == 1 ? 0 : KIT_DISABLED;
        kit_icon_button(x + fr.cancel.x, y + fr.cancel.y, s, UI_ICON_X, live | (live ? 0 : hot(x + fr.cancel.x, y + fr.cancel.y, s, s)));
        kit_icon_button(x + fr.enter.x, y + fr.enter.y, s, UI_ICON_CHECK, live | (live ? 0 : hot(x + fr.enter.x, y + fr.enter.y, s, s)));
        kit_vseparator(x + fr.enter.x + s + 6, y + fr.enter.y + 5, s - 10);
    }
    const SheetCell *active = &document()->cells[snapshot()->caret];
    const char *kind = active->kind == SHEET_TEXT ? "txt" : "fx";
    Clip kind_box = {fr.enter.x + s + 7, fr.name.y, fr.source.x - fr.enter.x - s - 7, s};
    if (box_visible(kind_box, w, h))
        kit_text_center(kind, x + kind_box.x, kit_ty(y + kind_box.y, s), kind_box.w, TK_TEXT2, 0);
    Clip source_ring = {fr.source.x - 2, fr.source.y - 2, fr.source.w + 4, fr.source.h + 4};
    if (fr.source.w >= 24 && box_visible(source_ring, w, h)) {
        kit_field(x + fr.source.x, y + fr.source.y, fr.source.w, fr.source.h, state.mode == 1 && !state.inactive ? KIT_FOCUS : 0);
        Clip box = field_text(fr.source); box.x += x; box.y += y;
        if (state.mode == 1) editor_draw(all, box);
        else label_n(intersect(all, box), active->text, active->length, box.x, box.y, TK_TEXT);
    }
}

static void draw_grid(Clip all, unsigned r0, unsigned c0, unsigned r1, unsigned c1) {
    int x = all.x, y = all.y;
    int grid_y = y + fr.grid_y, bottom = y + fr.bottom, right = x + fr.right, row_w = fr.row_w, head_h = fr.head_h;
    Clip grid = intersect(all, (Clip){x, y + fr.head_y, fr.right, fr.bottom - fr.head_y});
    Clip cells = intersect(grid, (Clip){x + row_w, grid_y, right - x - row_w, bottom - grid_y});
    uint8_t head = modern() ? TK_SUNKEN : TK_BASE, line = TK_LINE_SUBTLE;
    uint8_t picked = state.inactive ? TK_HOVER : TK_ACCENT_100, picked_ink = state.inactive ? TK_TEXT : TK_ACCENT_700;
    uint8_t edge = state.inactive ? TK_LINE_STRONG : TK_ACCENT, range_fill = state.inactive ? TK_SUNKEN : TK_ACCENT_50;
    /* Past column Z the grid shows the window content. */
    rect(grid, grid.x, grid.y, grid.w, grid.h, modern() ? ui_theme.content : TK_46);
    /* Corner: selects the whole sheet. */
    rect(grid, x, y + fr.head_y, row_w - 1, head_h - 1, head);
    rect(grid, x + row_w - 1, y + fr.head_y, 1, head_h, line); rect(grid, x, grid_y - 1, row_w, 1, line);
    for (int i = 0; i < 5; i++) rect(grid, x + row_w - 5 - i, grid_y - 5 + i - 4, i + 1, 1, TK_LINE_STRONG);
    /* Column headers: the selected range is tinted with an accent rule. */
    Clip heads = intersect(grid, (Clip){x + row_w, y + fr.head_y, right - x - row_w, head_h});
    for (unsigned c = state.first_col; c < SHEET_COLS; c++) {
        int cx = x + column_left(c), cw = (int)column_width(c); if (cx >= right) break;
        int in_range = c >= c0 && c <= c1;
        rect(heads, cx, y + fr.head_y, cw - 1, head_h - 1, in_range ? picked : head);
        rect(heads, cx + cw - 1, y + fr.head_y, 1, head_h, line); rect(heads, cx, grid_y - 1, cw, 1, line);
        if (in_range) rect(heads, cx, grid_y - 2, cw, 2, edge);
        char name[2] = {(char)('A' + c), 0};
        label(heads, name, cx + (cw - ui_advance(name[0])) / 2, kit_ty(y + fr.head_y, head_h - 1), in_range ? picked_ink : TK_TEXT2);
    }
    for (unsigned r = state.first_row; r < SHEET_ROWS; r++) {
        int ry = grid_y + (int)(r - state.first_row) * ROW_H; if (ry >= bottom) break;
        Clip row_head = intersect(grid, (Clip){x, ry, row_w, ROW_H});
        int in_rows = r >= r0 && r <= r1;
        rect(row_head, x, ry, row_w - 1, ROW_H - 1, in_rows ? picked : head);
        rect(row_head, x + row_w - 1, ry, 1, ROW_H, line); rect(row_head, x, ry + ROW_H - 1, row_w, 1, line);
        if (in_rows) rect(row_head, x + row_w - 2, ry, 2, ROW_H, edge);
        char number[11]; number_text(r + 1, number);
        label(row_head, number, x + row_w - ui_string_w(number) - 8, kit_ty(ry, ROW_H - 1), in_rows ? picked_ink : TK_TEXT2);
        for (unsigned c = state.first_col; c < SHEET_COLS; c++) {
            int cx = x + column_left(c), cw = (int)column_width(c); if (cx >= right) break;
            Clip cell = intersect(cells, (Clip){cx, ry, cw, ROW_H});
            int selected = r >= r0 && r <= r1 && c >= c0 && c <= c1, caret = r * SHEET_COLS + c == snapshot()->caret;
            rect(cell, cx, ry, cw - 1, ROW_H - 1, selected && !caret ? range_fill : TK_RAISED);
            rect(cell, cx + cw - 1, ry, 1, ROW_H, line); rect(cell, cx, ry + ROW_H - 1, cw, 1, line);
            Clip content = intersect(cell, (Clip){cx + 4, ry + 2, cw - 8, ROW_H - 4});
            int ty = kit_ty(ry, ROW_H - 1);
            if (caret && state.mode == 1) {
                /* The cell previews the source being edited in the formula bar. */
                label_n(content, state.edit, state.edit_length, cx + 5, ty, TK_TEXT);
                continue;
            }
            const SheetCell *v = sheet_cell(document(), r, c); char value[SHEET_TEXT_MAX + 1u]; unsigned length;
            if (!sheet_format_display(document(), r, c, value, sizeof value, &length)) {
                int tx = cx + 5, width = text_width(value, length);
                if (!v->error && (v->kind == SHEET_NUMBER || v->kind == SHEET_FORMULA)) {
                    tx = cx + cw - 6 - width;
                    /* Never turn a clipped negative/large value into a plausible
                     * different number, including partially visible columns. */
                    if (tx < content.x || tx + width > content.x + content.w) {
                        int advance = ui_advance('#');
                        length = advance > 0 && content.w > 2 ? min_u(3u, (unsigned)(content.w - 2) / (unsigned)advance) : 0;
                        for (unsigned i = 0; i < length; i++) value[i] = '#';
                        width = text_width(value, length); tx = content.x + content.w - width - 2;
                    }
                }
                label_n(content, value, length, tx, ty, v->error ? TK_RED_700 : TK_TEXT);
            }
        }
    }
    /* The range outline, then the active cell's 2px frame, over the grid. */
    int rx = x + column_left(c0), rw = (int)(column_start(c1 + 1) - column_start(c0));
    int ry = grid_y + ((int)r0 - (int)state.first_row) * ROW_H, rh = (int)(r1 - r0 + 1) * ROW_H;
    if (r0 != r1 || c0 != c1) frame(cells, rx - 1, ry - 1, rw + 1, rh + 1, edge);
    unsigned ar = snapshot()->caret / SHEET_COLS, ac = snapshot()->caret % SHEET_COLS;
    int ax = x + column_left(ac) - 1, ay = grid_y + ((int)ar - (int)state.first_row) * ROW_H - 1, aw = (int)column_width(ac) + 1;
    frame(cells, ax, ay, aw, ROW_H + 1, edge); frame(cells, ax + 1, ay + 1, aw - 2, ROW_H - 1, edge);
}

static void draw_status(Clip all, unsigned r0, unsigned c0, unsigned r1, unsigned c1) {
    int sh = fr.status_h, sy = all.y + fr.status_y, x = all.x, w = all.w;
    if (!box_visible((Clip){0, fr.status_y, w, sh}, all.w, all.h)) return;
    if (modern()) {
        draw_hline(x, sy, w, TK_LINE_SUBTLE);
        kit_fill_bottom(x, sy + 1, w, sh - 1, ui_theme.r_window - 1, ui_theme.statusbar);
    } else kit_statusbar(x, sy, w, sh);
    int pad = modern() ? 12 : 8, ty = kit_ty(sy + 1, sh - 1), left = x + pad, right = x + w - pad;
    char range[11]; sheet_label(r0, c0, range, 5);
    if (r0 != r1 || c0 != c1) { unsigned len = text_length(range); range[len++] = ':'; sheet_label(r1, c1, range + len, sizeof range - len); }
    int pending = state.save.pending, native = state.save.kind == DOCUMENT_SAVE_NATIVE;
    const char *saved = pending ? (native ? "Saving..." : "Exporting...") : spreadsheet_dirty() ? "Modified" : spreadsheet_file() >= 0 ? "Saved" : "";
    const char *mode = state.mode == 1 ? "Edit" : state.mode == 2 ? "Go to" : "Ready";
    const char *message = pending ? (native ? "Saving sheet to disk..." : "Exporting CSV to disk...") : state.status;
    int range_w = kit_text_w(range, 1);
    if (left + range_w < right) { kit_text(range, left, ty, TK_TEXT, range_w, 1); left += range_w + 16; }
    const char *items[2] = {saved, mode};
    for (unsigned i = 0; i < 2; i++) {
        int iw = kit_text_w(items[i], 0);
        if (!iw) continue;
        if (right - iw < left + 80) break;
        kit_text(items[i], right - iw, ty, TK_TEXT2, iw, 0);
        right -= iw + 16;
    }
    kit_text(message, left, ty, TK_TEXT2, right - left, 0);
}

void spreadsheet_draw(int x, int y, int w, int h) {
    if (!state.initialized) spreadsheet_init();
    if (w <= 0 || h <= 0) return;
    int resized = state.width != w || state.height != h;
    geometry(w, h);
    if (resized) reveal();
    Clip all = {x, y, w, h};
    unsigned r0, c0, r1, c1; selection(&r0, &c0, &r1, &c1);
    draw_toolbar(all, r0, c0, r1, c1);
    draw_formula_bar(all);
    draw_grid(all, r0, c0, r1, c1);
    draw_scrollbars(all);
    draw_status(all, r0, c0, r1, c1);
}

static unsigned hit_cell(int mx, int my) {
    int px = clamp(mx, fr.row_w, fr.right > fr.row_w ? fr.right - 1 : fr.row_w);
    int py = clamp(my, fr.grid_y, fr.bottom > fr.grid_y ? fr.bottom - 1 : fr.grid_y);
    unsigned col = column_at(state.scroll_x + (unsigned)(px - fr.row_w));
    unsigned row = min_u(state.first_row + (unsigned)(py - fr.grid_y) / ROW_H, SHEET_ROWS - 1);
    return row * SHEET_COLS + col;
}
static void editor_hit(int mx, int left, int width, int extend) {
    int wanted = mx - left + editor_offset(width), used = 0; unsigned at = 0;
    while (at < state.edit_length) {
        int advance = ui_advance(visible_char(state.edit[at]));
        if (wanted < used + (advance + 1) / 2) break;
        used += advance; at++;
    }
    state.edit_caret = at; if (!extend) state.edit_anchor = at; state.blink = 1;
}
/* Thumb drags map the pointer back through the track; grab < 0 marks a
 * track click, which does not repeat while the button is held. */
static void thumb_drag(int mx, int my, int vertical) {
    int at, size; thumb(vertical, &at, &size);
    if (state.grab < 0) return;
    int travel = (vertical ? fr.bottom - fr.grid_y : fr.right - fr.row_w) - 4 - size;
    int offset = (vertical ? my - fr.grid_y : mx - fr.row_w) - 2 - state.grab;
    if (travel <= 0) return;
    offset = clamp(offset, 0, travel);
    if (vertical) state.first_row = (unsigned)(offset * (int)(SHEET_ROWS - state.visible_rows) / travel);
    else horizontal_scroll((unsigned)offset * max_scroll_x() / (unsigned)travel);
}
static void scrollbar_hit(int mx, int my, int vertical) {
    int at, size; thumb(vertical, &at, &size);
    int offset = (vertical ? my - fr.grid_y : mx - fr.row_w) - 2;
    if (offset >= at && offset < at + size) { state.grab = offset - at; return; }
    state.grab = -1;
    if (vertical) spreadsheet_scroll(offset < at ? -(int)state.visible_rows : (int)state.visible_rows);
    else scroll_column(offset < at ? -1 : 1);
}
int spreadsheet_click(int x, int y, int w, int h, int mx, int my, int modifiers) {
    if (!state.initialized) spreadsheet_init();
    if (w <= 0 || h <= 0 || mx < x || my < y || mx >= x + w || my >= y + h) return 0;
    geometry(w, h); mx -= x; my -= y; state.dragging = 0;
    Toolbar bar; toolbar_layout(w, &bar);
    for (unsigned i = 0; i < TOOL_COUNT; i++) {
        Clip b = bar.item[i];
        if (!box_visible(b, w, h) || mx < b.x || mx >= b.x + b.w || my < b.y || my >= b.y + b.h) continue;
        if (i == TOOL_SAVE) return SPREADSHEET_REQUEST_SAVE;
        if (i == TOOL_SAVE_AS) return SPREADSHEET_REQUEST_SAVE_AS;
        if (i == TOOL_CSV) return SPREADSHEET_REQUEST_EXPORT;
        if (i == TOOL_UNDO || i == TOOL_REDO) return undo(i == TOOL_REDO);
        if (i >= TOOL_GENERAL && i <= TOOL_PERCENT) return format_selection((SheetFormat)(i - TOOL_GENERAL));
        if (i == TOOL_NARROWER || i == TOOL_WIDER) return resize_columns(i == TOOL_NARROWER ? -16 : 16);
        if (i == TOOL_EDIT) { if (commit_edit()) start_edit(0); }
        else open_address();
        return SPREADSHEET_CHANGED;
    }
    if (my < fr.tool_h) return 0;
    if (my < fr.head_y) {
        Clip box = field_text(fr.source);
        if (box_visible(fr.name, w, h) && mx >= fr.name.x && mx < fr.name.x + fr.name.w && my >= fr.name.y && my < fr.name.y + fr.name.h) {
            open_address(); return SPREADSHEET_CHANGED;
        }
        if (state.mode == 1 && box_visible(fr.enter, w, h) && my >= fr.cancel.y && my < fr.cancel.y + fr.cancel.h) {
            if (mx >= fr.cancel.x && mx < fr.cancel.x + fr.cancel.w) { cancel_edit(); return SPREADSHEET_CHANGED; }
            if (mx >= fr.enter.x && mx < fr.enter.x + fr.enter.w) { commit_edit(); return SPREADSHEET_CHANGED; }
        }
        if (fr.source.w >= 24 && mx >= fr.source.x && mx < fr.source.x + fr.source.w && my >= fr.source.y && my < fr.source.y + fr.source.h) {
            if (state.mode != 1) start_edit(0);
            editor_hit(mx, box.x, box.w, modifiers & SPREADSHEET_MOD_SHIFT);
            state.dragging = 1; state.drag_kind = 4; return SPREADSHEET_CHANGED;
        }
        return 0;
    }
    int top = fr.grid_y, bottom = fr.bottom, right = fr.right;
    if (mx >= right && my >= top && my < bottom) {
        scrollbar_hit(mx, my, 1); state.dragging = 1; state.drag_kind = 2; return SPREADSHEET_CHANGED;
    }
    if (my >= bottom && my < bottom + fr.sb && mx >= fr.row_w && mx < right) {
        scrollbar_hit(mx, my, 0); state.dragging = 1; state.drag_kind = 3; return SPREADSHEET_CHANGED;
    }
    if (my >= bottom || mx >= right) return 0;
    if (!commit_edit()) return SPREADSHEET_CHANGED;
    if (state.mode == 2) state.mode = 0;
    unsigned at = hit_cell(mx, my); int extend = modifiers & SPREADSHEET_MOD_SHIFT;
    if (mx < fr.row_w && my < top) {
        snapshot()->anchor = 0; snapshot()->caret = SHEET_CELLS - 1; state.click_valid = 0; return SPREADSHEET_CHANGED;
    }
    if (my < top) {
        snapshot()->anchor = at % SHEET_COLS; snapshot()->caret = (SHEET_ROWS - 1) * SHEET_COLS + at % SHEET_COLS;
        state.drag_kind = 6;
    } else if (mx < fr.row_w) {
        snapshot()->anchor = at / SHEET_COLS * SHEET_COLS; snapshot()->caret = at / SHEET_COLS * SHEET_COLS + SHEET_COLS - 1;
        state.drag_kind = 5;
    } else {
        int double_click = state.click_valid && state.click_cell == at && timer_ticks() - state.click_tick < TIMER_HZ / 2 && !extend;
        move_to(at / SHEET_COLS, at % SHEET_COLS, extend);
        if (double_click) { start_edit(0); state.click_valid = 0; return SPREADSHEET_CHANGED; }
        state.click_valid = 1; state.click_cell = at; state.click_tick = timer_ticks(); state.drag_kind = 1;
    }
    state.dragging = 1; return SPREADSHEET_CHANGED;
}
int spreadsheet_drag(int x, int y, int w, int h, int mx, int my) {
    if (!state.initialized || !state.dragging || w <= 0 || h <= 0) return 0;
    geometry(w, h); mx -= x; my -= y;
    if (state.drag_kind == 4) { Clip box = field_text(fr.source); editor_hit(mx, box.x, box.w, 1); return SPREADSHEET_CHANGED; }
    if (state.drag_kind == 2 || state.drag_kind == 3) { thumb_drag(mx, my, state.drag_kind == 2); return SPREADSHEET_CHANGED; }
    if (state.drag_kind != 6) {
        if (my < fr.grid_y) spreadsheet_scroll(-1);
        else if (my >= fr.bottom) spreadsheet_scroll(1);
    }
    if (state.drag_kind != 5) {
        if (mx < fr.row_w) scroll_column(-1);
        else if (mx >= fr.right) scroll_column(1);
    }
    unsigned at = hit_cell(mx, my);
    if (state.drag_kind == 5) at = at / SHEET_COLS * SHEET_COLS + SHEET_COLS - 1;
    else if (state.drag_kind == 6) at = (SHEET_ROWS - 1) * SHEET_COLS + at % SHEET_COLS;
    if (at != snapshot()->caret) state.click_valid = 0;
    snapshot()->caret = at; return SPREADSHEET_CHANGED;
}
