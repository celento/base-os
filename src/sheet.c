/* Bounded desktop Spreadsheet. Large storage belongs to its reserved arena. */
#include "sheet.h"
#include "gfx.h"
#include "fs.h"
#include "layout.h"
#include "platform.h"
#include "../sdk/baseos_abi.h"

#ifndef SHEET_BASE
#define SHEET_BASE 0x720000
#endif
#ifndef SHEET_CAPACITY
#define SHEET_CAPACITY 0x2E0000
#endif
#define HISTORY 5u
#define TOOL_H 68
#define FORMULA_H 34
#define HEADER_H 24
#define FOOT_H 28
#define SCROLL_H 14
#define ROW_H 24
#define ROW_W 42
#define OUTPUT_CAPACITY SHEET_CSV_MAX_SIZE

typedef struct { SheetDoc doc; unsigned caret, anchor; DocumentRevision revision; } Snapshot;
typedef struct {
    int valid, parent;
    unsigned parent_identity;
    DocumentTarget target;
    DocumentRevision revision;
    char name[FS_NAME_LEN];
} SpreadsheetExportRetry;
_Static_assert(sizeof(SpreadsheetExportRetry) == 64u, "Spreadsheet export retry budget changed");
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
    return state.width > ROW_W + SCROLL_H ? (unsigned)(state.width - ROW_W - SCROLL_H) : 1u;
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
static int column_left(unsigned col) { return ROW_W + (int)column_start(col) - (int)state.scroll_x; }
static void geometry(int w, int h) {
    state.width = w; state.height = h;
    state.visible_rows = (unsigned)max_u(1u, h > TOOL_H + FORMULA_H + HEADER_H + FOOT_H + SCROLL_H ?
        (unsigned)(h - TOOL_H - FORMULA_H - HEADER_H - FOOT_H - SCROLL_H) / ROW_H : 1u);
    state.visible_rows = min_u(state.visible_rows, SHEET_ROWS);
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
 * primitive and antialiased glyph below is intersected with the client/cell. */
typedef struct { int x, y, w, h; } Clip;
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
typedef struct { int x, w; const char *text; } Button;
static const Button buttons[] = {
    {8, 48, "Save"}, {60, 62, "Save As"}, {126, 42, "CSV"}, {172, 46, "Undo"}, {222, 46, "Redo"}, {272, 38, "Edit"}, {314, 34, "Go"}
};
static const Button format_buttons[] = {
    {8, 60, "General"}, {72, 48, "0.00"}, {124, 48, "$0.00"}, {176, 32, "%"}, {255, 24, "-"}, {323, 24, "+"}
};
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
        if (selected) rect(box, x, box.y, width, box.h, COLOR_BLUE);
        glyph(box, state.edit[i], x, box.y, selected ? COLOR_WHITE : COLOR_DKGRAY); x += width;
    }
    if (state.blink && lo == hi) {
        x = box.x - editor_offset(box.w) + text_width(state.edit, state.edit_caret);
        rect(box, x, box.y, 1, box.h, COLOR_BLACK);
    }
}
int spreadsheet_cell_position(unsigned row, unsigned col, int *x, int *y, int *w, int *h) {
    if (!state.initialized || row < state.first_row || col < state.first_col || row >= SHEET_ROWS || col >= SHEET_COLS) return 0;
    int left = column_left(col), top = TOOL_H + FORMULA_H + HEADER_H + (int)(row - state.first_row) * ROW_H;
    int right = left + (int)column_width(col), bottom = top + ROW_H;
    if (left < ROW_W) left = ROW_W;
    if (right > state.width - SCROLL_H) right = state.width - SCROLL_H;
    if (bottom > state.height - FOOT_H - SCROLL_H) bottom = state.height - FOOT_H - SCROLL_H;
    if (left >= right || top >= bottom) return 0;
    if (x) *x = left;
    if (y) *y = top;
    if (w) *w = right - left;
    if (h) *h = bottom - top;
    return 1;
}
static void draw_scrollbars(Clip all) {
    int top = all.y + TOOL_H + FORMULA_H + HEADER_H, bottom = all.y + all.h - FOOT_H - SCROLL_H;
    int right = all.x + all.w - SCROLL_H;
    unsigned char track = gfx_rgb(230, 234, 240), thumb = gfx_rgb(153, 168, 187);
    rect(all, right, top, SCROLL_H, bottom - top, track);
    rect(all, all.x + ROW_W, bottom, right - all.x - ROW_W, SCROLL_H, track);
    int vh = bottom - top - 2 * SCROLL_H, hw = right - all.x - ROW_W - 2 * SCROLL_H;
    if (vh > 0) {
        int size = (int)(state.visible_rows * (unsigned)vh / SHEET_ROWS); if (size < 14) size = 14; if (size > vh) size = vh;
        int pos = state.visible_rows < SHEET_ROWS ? (int)(state.first_row * (unsigned)(vh - size) / (SHEET_ROWS - state.visible_rows)) : 0;
        rect(all, right + 2, top + SCROLL_H + pos, SCROLL_H - 4, size, thumb);
        label(all, "^", right + 3, top - 1, COLOR_DKGRAY); label(all, "v", right + 3, bottom - SCROLL_H - 2, COLOR_DKGRAY);
    }
    if (hw > 0) {
        unsigned total = column_start(SHEET_COLS), maximum = max_scroll_x();
        int size = (int)(min_u(grid_width(), total) * (unsigned)hw / total); if (size < 14) size = 14; if (size > hw) size = hw;
        int pos = maximum ? (int)(state.scroll_x * (unsigned)(hw - size) / maximum) : 0;
        rect(all, all.x + ROW_W + SCROLL_H + pos, bottom + 2, size, SCROLL_H - 4, thumb);
        label(all, "<", all.x + ROW_W + 3, bottom - 2, COLOR_DKGRAY); label(all, ">", right - SCROLL_H + 3, bottom - 2, COLOR_DKGRAY);
    }
}
void spreadsheet_draw(int x, int y, int w, int h) {
    if (!state.initialized) spreadsheet_init();
    if (w <= 0 || h <= 0) return;
    int resized = state.width != w || state.height != h;
    geometry(w, h);
    if (resized) reveal();
    Clip all = {x, y, w, h}; unsigned char panel = gfx_rgb(237, 241, 247), line = gfx_rgb(215, 222, 231);
    rect(all, x, y, w, h, COLOR_WHITE); rect(all, x, y, w, TOOL_H + FORMULA_H, panel);
    for (unsigned i = 0; i < sizeof buttons / sizeof buttons[0]; i++) {
        const Button *b = &buttons[i];
        rect(all, x + b->x, y + 5, b->w, 24, COLOR_WHITE);
        frame(all, x + b->x, y + 5, b->w, 24, line);
        label(all, b->text, x + b->x + (b->w - ui_string_w(b->text)) / 2, y + 8, COLOR_DKGRAY);
    }
    if (w >= 480) label(all, "Spreadsheet", x + 366, y + 8, COLOR_DKGRAY);
    unsigned r0, c0, r1, c1; selection(&r0, &c0, &r1, &c1);
    int uniform_format = document()->formats[r0 * SHEET_COLS + c0];
    for (unsigned r = r0; r <= r1 && uniform_format >= 0; r++)
        for (unsigned c = c0; c <= c1; c++)
            if (document()->formats[r * SHEET_COLS + c] != uniform_format) { uniform_format = -1; break; }
    for (unsigned i = 0; i < sizeof format_buttons / sizeof format_buttons[0]; i++) {
        const Button *b = &format_buttons[i];
        int selected = i < 4 && (int)i == uniform_format;
        rect(all, x + b->x, y + 39, b->w, 24, selected ? gfx_rgb(211, 225, 246) : COLOR_WHITE);
        frame(all, x + b->x, y + 39, b->w, 24, selected ? COLOR_BLUE : line);
        label(all, b->text, x + b->x + (b->w - ui_string_w(b->text)) / 2, y + 42, COLOR_DKGRAY);
    }
    label(all, "Width", x + 214, y + 42, COLOR_DKGRAY);
    unsigned width = column_width(c0); int mixed = 0;
    for (unsigned c = c0 + 1; c <= c1; c++) if (column_width(c) != width) mixed = 1;
    char width_text[11]; number_text(width, width_text);
    label(intersect(all, (Clip){x + 283, y + 42, 36, 18}), mixed ? "..." : width_text, x + 283, y + 42, COLOR_DKGRAY);
    if (w >= 610) label(all, "Ctrl+1..4 format; Ctrl+0 resets width", x + 366, y + 42, COLOR_DKGRAY);
    char address[5]; sheet_label(snapshot()->caret / SHEET_COLS, snapshot()->caret % SHEET_COLS, address, sizeof address);
    rect(all, x + 8, y + TOOL_H + 3, 70, 27, state.mode == 2 ? COLOR_BLUE : line);
    rect(all, x + 9, y + TOOL_H + 4, 68, 25, COLOR_WHITE);
    Clip address_box = {x + 14, y + TOOL_H + 7, 58, 18};
    if (state.mode == 2) editor_draw(all, address_box);
    else label(intersect(all, address_box), address, address_box.x, address_box.y, COLOR_DKGRAY);
    const SheetCell *active = &document()->cells[snapshot()->caret];
    label(all, active->kind == SHEET_TEXT ? "txt" : "fx", x + 85, y + TOOL_H + 7, COLOR_DKGRAY);
    rect(all, x + 111, y + TOOL_H + 3, w - 119, 27, state.mode == 1 ? COLOR_BLUE : line);
    rect(all, x + 112, y + TOOL_H + 4, w - 121, 25, COLOR_WHITE);
    Clip source = {x + 117, y + TOOL_H + 7, w - 131, 18};
    if (state.mode == 1) editor_draw(all, source);
    else label_n(intersect(all, source), active->text, active->length, source.x, source.y, COLOR_DKGRAY);
    int grid_y = y + TOOL_H + FORMULA_H + HEADER_H, bottom = y + h - FOOT_H - SCROLL_H, right = x + w - SCROLL_H;
    Clip grid = intersect(all, (Clip){x, y + TOOL_H + FORMULA_H, w - SCROLL_H, bottom - y - TOOL_H - FORMULA_H});
    rect(grid, x, grid.y, ROW_W, HEADER_H, panel);
    for (unsigned c = state.first_col; c < SHEET_COLS; c++) {
        int cx = x + column_left(c), cw = (int)column_width(c); if (cx >= right) break;
        Clip head = intersect(intersect(grid, (Clip){x + ROW_W, grid.y, right - x - ROW_W, grid.h}), (Clip){cx, grid_y - HEADER_H, cw, HEADER_H});
        rect(head, cx, grid_y - HEADER_H, cw, HEADER_H, c >= c0 && c <= c1 ? gfx_rgb(211, 225, 246) : panel);
        char name[2] = {(char)('A' + c), 0}; label(head, name, cx + (cw - ui_advance(name[0])) / 2, grid_y - HEADER_H + 3, COLOR_DKGRAY);
        frame(head, cx, grid_y - HEADER_H, cw, HEADER_H, line);
    }
    for (unsigned r = state.first_row; r < SHEET_ROWS; r++) {
        int ry = grid_y + (int)(r - state.first_row) * ROW_H; if (ry >= bottom) break;
        Clip head = intersect(grid, (Clip){x, ry, ROW_W, ROW_H});
        rect(head, x, ry, ROW_W, ROW_H, r >= r0 && r <= r1 ? gfx_rgb(211, 225, 246) : panel);
        char number[11]; number_text(r + 1, number); label(head, number, x + ROW_W - ui_string_w(number) - 6, ry + 3, COLOR_DKGRAY);
        frame(head, x, ry, ROW_W, ROW_H, line);
        for (unsigned c = state.first_col; c < SHEET_COLS; c++) {
            int cx = x + column_left(c), cw = (int)column_width(c); if (cx >= right) break;
            Clip cell = intersect(intersect(grid, (Clip){x + ROW_W, grid.y, right - x - ROW_W, grid.h}), (Clip){cx, ry, cw, ROW_H});
            int selected = r >= r0 && r <= r1 && c >= c0 && c <= c1;
            rect(cell, cx, ry, cw, ROW_H, selected ? gfx_rgb(229, 239, 253) : COLOR_WHITE);
            frame(cell, cx, ry, cw, ROW_H, line);
            const SheetCell *v = sheet_cell(document(), r, c); char value[SHEET_TEXT_MAX + 1u]; unsigned length;
            if (!sheet_format_display(document(), r, c, value, sizeof value, &length)) {
                int tx = cx + 5, width = text_width(value, length);
                Clip content = intersect(cell, (Clip){cx + 4, ry + 2, cw - 8, ROW_H - 4});
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
                label_n(content, value, length, tx, ry + 3, v->error ? COLOR_RED : COLOR_DKGRAY);
            }
            if (r * SHEET_COLS + c == snapshot()->caret) {
                frame(cell, cx, ry, cw, ROW_H, COLOR_BLUE); frame(cell, cx + 1, ry + 1, cw - 2, ROW_H - 2, COLOR_BLUE);
            }
        }
    }
    draw_scrollbars(all);
    rect(all, x, y + h - FOOT_H, w, FOOT_H, panel);
    char range[11]; sheet_label(r0, c0, range, 5);
    if (r0 != r1 || c0 != c1) { unsigned len = text_length(range); range[len++] = ':'; sheet_label(r1, c1, range + len, sizeof range - len); }
    Clip foot = intersect(all, (Clip){x + 8, y + h - FOOT_H + 5, w - 16, 18});
    label(foot, range, foot.x, foot.y, COLOR_BLUE);
    int sx = foot.x + ui_string_w(range) + 14;
    const char *message = state.save.pending ?
        (state.save.kind == DOCUMENT_SAVE_NATIVE ? "Saving sheet to disk..." : "Exporting CSV to disk...") : state.status;
    label(foot, message, sx, foot.y, COLOR_DKGRAY);
}

static unsigned hit_cell(int w, int h, int mx, int my) {
    int right = w - SCROLL_H, bottom = h - FOOT_H - SCROLL_H;
    int px = clamp(mx, ROW_W, right > ROW_W ? right - 1 : ROW_W);
    int py = clamp(my, TOOL_H + FORMULA_H + HEADER_H, bottom > TOOL_H + FORMULA_H + HEADER_H ? bottom - 1 : TOOL_H + FORMULA_H + HEADER_H);
    unsigned col = column_at(state.scroll_x + (unsigned)(px - ROW_W));
    unsigned row = min_u(state.first_row + (unsigned)(py - TOOL_H - FORMULA_H - HEADER_H) / ROW_H, SHEET_ROWS - 1);
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
static void scrollbar_hit(int w, int h, int mx, int my, int vertical) {
    if (vertical) {
        int start = TOOL_H + FORMULA_H + HEADER_H, end = h - FOOT_H - SCROLL_H;
        if (my < start + SCROLL_H) spreadsheet_scroll(-1);
        else if (my >= end - SCROLL_H) spreadsheet_scroll(1);
        else {
            int span = end - start - 2 * SCROLL_H;
            state.first_row = span > 0 ? (unsigned)clamp((my - start - SCROLL_H) * (int)(SHEET_ROWS - state.visible_rows) / span, 0, SHEET_ROWS - state.visible_rows) : 0;
        }
    } else {
        int start = ROW_W, end = w - SCROLL_H;
        if (mx < start + SCROLL_H) scroll_column(-1);
        else if (mx >= end - SCROLL_H) scroll_column(1);
        else {
            unsigned total = column_start(SHEET_COLS), maximum = max_scroll_x();
            int span = end - start - 2 * SCROLL_H;
            int size = span > 0 ? (int)(min_u(grid_width(), total) * (unsigned)span / total) : 0;
            if (size < 14) size = 14;
            if (size > span) size = span;
            int travel = span - size;
            int offset = clamp(mx - start - SCROLL_H - size / 2, 0, travel > 0 ? travel : 0);
            horizontal_scroll(travel > 0 ? (unsigned)offset * maximum / (unsigned)travel : 0);
        }
    }
}
int spreadsheet_click(int x, int y, int w, int h, int mx, int my, int modifiers) {
    if (!state.initialized) spreadsheet_init();
    if (w <= 0 || h <= 0 || mx < x || my < y || mx >= x + w || my >= y + h) return 0;
    geometry(w, h); mx -= x; my -= y; state.dragging = 0;
    if (my >= 5 && my < 29) {
        for (unsigned i = 0; i < sizeof buttons / sizeof buttons[0]; i++) if (mx >= buttons[i].x && mx < buttons[i].x + buttons[i].w) {
            if (i == 0) return SPREADSHEET_REQUEST_SAVE;
            if (i == 1) return SPREADSHEET_REQUEST_SAVE_AS;
            if (i == 2) return SPREADSHEET_REQUEST_EXPORT;
            if (i == 3 || i == 4) return undo(i == 4);
            if (i == 5) { if (commit_edit()) start_edit(0); }
            else open_address();
            return SPREADSHEET_CHANGED;
        }
    }
    if (my >= 39 && my < 63) {
        for (unsigned i = 0; i < sizeof format_buttons / sizeof format_buttons[0]; i++)
            if (mx >= format_buttons[i].x && mx < format_buttons[i].x + format_buttons[i].w)
                return i < 4 ? format_selection((SheetFormat)i) : resize_columns(i == 4 ? -16 : 16);
    }
    if (my >= TOOL_H + 3 && my < TOOL_H + FORMULA_H - 4) {
        if (mx >= 8 && mx < 78) { open_address(); return SPREADSHEET_CHANGED; }
        if (mx >= 111 && mx < w - 8) {
            if (state.mode != 1) start_edit(0);
            editor_hit(mx, 117, w - 131, modifiers & SPREADSHEET_MOD_SHIFT);
            state.dragging = 1; state.drag_kind = 4; return SPREADSHEET_CHANGED;
        }
    }
    int top = TOOL_H + FORMULA_H + HEADER_H, bottom = h - FOOT_H - SCROLL_H, right = w - SCROLL_H;
    if (mx >= right && my >= top && my < bottom) {
        scrollbar_hit(w, h, mx, my, 1); state.dragging = 1; state.drag_kind = 2; return SPREADSHEET_CHANGED;
    }
    if (my >= bottom && my < bottom + SCROLL_H && mx >= ROW_W && mx < right) {
        scrollbar_hit(w, h, mx, my, 0); state.dragging = 1; state.drag_kind = 3; return SPREADSHEET_CHANGED;
    }
    if (my < TOOL_H + FORMULA_H || my >= bottom || mx >= right) return 0;
    if (!commit_edit()) return SPREADSHEET_CHANGED;
    if (state.mode == 2) state.mode = 0;
    unsigned at = hit_cell(w, h, mx, my); int extend = modifiers & SPREADSHEET_MOD_SHIFT;
    if (mx < ROW_W && my < top) {
        snapshot()->anchor = 0; snapshot()->caret = SHEET_CELLS - 1; state.click_valid = 0; return SPREADSHEET_CHANGED;
    }
    if (my < top) {
        snapshot()->anchor = at % SHEET_COLS; snapshot()->caret = (SHEET_ROWS - 1) * SHEET_COLS + at % SHEET_COLS;
        state.drag_kind = 6;
    } else if (mx < ROW_W) {
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
    if (state.drag_kind == 4) { editor_hit(mx, 117, w - 131, 1); return SPREADSHEET_CHANGED; }
    if (state.drag_kind == 2 || state.drag_kind == 3) { scrollbar_hit(w, h, mx, my, state.drag_kind == 2); return SPREADSHEET_CHANGED; }
    if (state.drag_kind != 6) {
        if (my < TOOL_H + FORMULA_H + HEADER_H) spreadsheet_scroll(-1);
        else if (my >= h - FOOT_H - SCROLL_H) spreadsheet_scroll(1);
    }
    if (state.drag_kind != 5) {
        if (mx < ROW_W) scroll_column(-1);
        else if (mx >= w - SCROLL_H) scroll_column(1);
    }
    unsigned at = hit_cell(w, h, mx, my);
    if (state.drag_kind == 5) at = at / SHEET_COLS * SHEET_COLS + SHEET_COLS - 1;
    else if (state.drag_kind == 6) at = (SHEET_ROWS - 1) * SHEET_COLS + at % SHEET_COLS;
    if (at != snapshot()->caret) state.click_valid = 0;
    snapshot()->caret = at; return SPREADSHEET_CHANGED;
}
