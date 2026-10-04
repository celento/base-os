/* Bounded rich-text Writer. All persistent working storage lives in its arena. */
#include "writer.h"
#include "gfx.h"
#include "fs.h"
#include "layout.h"
#include "platform.h"

#define HISTORY 9u
#define LINE_MAX (WRITER_TEXT_MAX + 1u)
#define EXPORT_CAPACITY 524288u
#define TOOL_H 66
#define FOOT_H 26
#define PAGE_PAD 22
#define UI_HEIGHT 18
#define BODY_HEIGHT 24
#define HEADING_HEIGHT 38

typedef struct {
    WriterDoc doc;
    unsigned caret, anchor, revision, typing_style, affinity;
} Snapshot;
typedef struct {
    unsigned y;
    unsigned short start, end, width;
    unsigned char height, paragraph;
} Line;
typedef struct {
    Snapshot history[HISTORY];
    WriterDoc staging, clipboard;
    Line lines[LINE_MAX];
    unsigned char output[EXPORT_CAPACITY];
} WriterArena;
_Static_assert(sizeof(Line) == 12, "Writer line bound changed");
_Static_assert(sizeof(WriterArena) <= WRITER_CAPACITY, "Writer arena overflow");
#ifdef WRITER_HOST_TEST
static WriterArena host_arena;
#define ARENA (&host_arena)
#else
#define ARENA ((WriterArena *)WRITER_BASE)
#endif

static struct {
    unsigned first, count, current, next_revision, saved_revision;
    unsigned clipboard_generation, clipboard_length;
    unsigned line_count, layout_revision;
    int initialized, file, failed_save, clipboard_owned, dragging;
    unsigned identity;
    WriterBinding binding;
    int has_binding, binding_conflict;
    struct { int valid, file; unsigned identity, revision, paper, length; } pending_pdf;
    unsigned group_kind, group_caret, group_ticks, group_count;
    unsigned stats_revision, words;
    struct {
        int open, replacing, focus, field, exact;
        unsigned caret[2], anchor[2];
        char text[2][64];
    } search;
    int width, content_height, scroll, viewport_height, desired_x, manual_scroll;
    int layout_valid, blink, last_blink, hit_affinity;
    char title[64], status[112];
} state;

/* Default clipboard is local; the desktop supplies shared generation hooks. */
__attribute__((weak)) unsigned writer_clipboard_set(const char *text, unsigned length) {
    (void)text; (void)length; return 0;
}
__attribute__((weak)) int writer_clipboard_get(char *text, unsigned capacity, unsigned *generation) {
    (void)text; (void)capacity; (void)generation; return -1;
}
static void copy_bytes(void *out, const void *in, unsigned length) {
    unsigned char *d = out; const unsigned char *s = in;
    for (unsigned i = 0; i < length; i++) {
        d[i] = s[i];
        if ((i & 4095u) == 4095u) platform_poll();
    }
}
static void text_copy(char *out, unsigned capacity, const char *in) {
    unsigned i = 0;
    if (!capacity) return;
    if (in) while (i + 1 < capacity && in[i]) { out[i] = in[i]; i++; }
    out[i] = 0;
}
static void status(const char *message) { text_copy(state.status, sizeof(state.status), message); }
static int clamp(int n, int a, int b) { return n < a ? a : n > b ? b : n; }
static unsigned min_u(unsigned a, unsigned b) { return a < b ? a : b; }
static Snapshot *snapshot(void) { return &ARENA->history[(state.first + state.current) % HISTORY]; }
static WriterDoc *document(void) { return &snapshot()->doc; }
static unsigned selection_low(void) { return min_u(snapshot()->caret, snapshot()->anchor); }
static unsigned selection_high(void) { return snapshot()->caret > snapshot()->anchor ? snapshot()->caret : snapshot()->anchor; }
static unsigned paragraph_start(const WriterDoc *d, unsigned at) {
    while (at && d->text[at - 1] != '\n') at--;
    return at;
}
static unsigned paragraph_end(const WriterDoc *d, unsigned at) {
    while (at < d->length && d->text[at] != '\n') at++;
    return at;
}
static int is_start(const WriterDoc *d, unsigned at) { return !at || d->text[at - 1] == '\n'; }
static unsigned paragraph_at(const WriterDoc *d, unsigned at) { return d->paragraph[paragraph_start(d, at)]; }
static void reveal(void);
static void invalidate(void) { state.layout_valid = 0; state.desired_x = -1; state.blink = 1; }

/* Each complete operation records one state. Older states drop as a ring. */
static Snapshot *begin_edit(void) {
    state.group_kind = 0; state.manual_scroll = 0;
    unsigned old = (state.first + state.current) % HISTORY;
    state.count = state.current + 1;
    if (state.count == HISTORY) {
        state.first = (state.first + 1) % HISTORY;
        state.current--; state.count--;
    }
    state.current++; state.count++;
    Snapshot *s = snapshot();
    copy_bytes(s, &ARENA->history[old], sizeof(*s));
    s->revision = ++state.next_revision;
    invalidate();
    return s;
}
static void accept_staging(unsigned caret, unsigned typing_style, unsigned group) {
    unsigned now = timer_ticks();
    int join = group && state.group_kind == group && state.group_caret == snapshot()->caret &&
               snapshot()->caret == snapshot()->anchor && state.current + 1 == state.count &&
               snapshot()->revision != state.saved_revision && state.group_count < 512u &&
               now - state.group_ticks < TIMER_HZ;
    unsigned count = join ? state.group_count + 1 : 1;
    Snapshot *s;
    if (join) { s = snapshot(); s->revision = ++state.next_revision; invalidate(); }
    else s = begin_edit();
    copy_bytes(&s->doc, &ARENA->staging, sizeof(WriterDoc));
    s->caret = s->anchor = caret; s->affinity = 0;
    s->typing_style = typing_style & WRITER_STYLE_MASK;
    state.group_kind = group; state.group_caret = caret; state.group_ticks = now; state.group_count = count;
    status("Edited. Ctrl+S saves the native document.");
}
static int undo(int redo) {
    state.group_kind = 0; state.manual_scroll = 0;
    if ((!redo && !state.current) || (redo && state.current + 1 >= state.count)) {
        status(redo ? "Nothing to redo." : "Nothing to undo."); return WRITER_CHANGED;
    }
    state.current += redo ? 1 : (unsigned)-1;
    invalidate(); status(redo ? "Redone." : "Undone.");
    return WRITER_CHANGED;
}

/* Replacement builds a canonical complete candidate before publishing it.
 * Pasted first-paragraph attributes apply only at a paragraph boundary; newly
 * inserted paragraphs retain rich clipboard attributes. The destination's
 * partial first paragraph retains its existing paragraph properties. */
static int replace(unsigned lo, unsigned hi, const unsigned char *text, unsigned length,
                   const unsigned char *styles, const unsigned char *paragraphs, unsigned group) {
    WriterDoc *d = document(), *n = &ARENA->staging;
    if (lo > hi || hi > d->length || length > WRITER_TEXT_MAX - (d->length - (hi - lo))) {
        status("Document limit is 32,768 text bytes. Nothing was changed."); return 0;
    }
    for (unsigned i = 0; i < length; i++) {
        unsigned c = text[i];
        if ((c < 32 && c != '\n' && c != '\t') || c > 126) {
            status("Only printable ASCII, tabs and LF line breaks are supported."); return 0;
        }
    }
    unsigned typing = styles && paragraphs ? styles[length] : snapshot()->typing_style;
    unsigned inherited = paragraph_at(d, lo);
    n->length = d->length - (hi - lo) + length;
    for (unsigned i = 0; i <= n->length; i++) n->paragraph[i] = 0;
    for (unsigned i = 0; i < lo; i++) {
        n->text[i] = d->text[i]; n->style[i] = d->style[i]; n->paragraph[i] = d->paragraph[i];
    }
    for (unsigned i = 0; i < length; i++) {
        n->text[lo + i] = text[i];
        n->style[lo + i] = styles ? styles[i] : (unsigned char)typing;
        if (i && text[i - 1] == '\n') n->paragraph[lo + i] = paragraphs ? paragraphs[i] : (unsigned char)inherited;
    }
    for (unsigned i = hi; i < d->length; i++) {
        unsigned dest = lo + length + i - hi;
        n->text[dest] = d->text[i]; n->style[dest] = d->style[i];
        if (i > hi && is_start(d, i)) n->paragraph[dest] = d->paragraph[i];
    }
    /* The join may create a paragraph boundary in front of the old suffix. */
    if (is_start(n, lo)) n->paragraph[lo] = (unsigned char)(paragraphs && length ? paragraphs[0] : inherited);
    if (is_start(n, lo + length)) {
        unsigned value = length && paragraphs ? paragraphs[length] : inherited;
        if (length && text[length - 1] != '\n') value = inherited;
        n->paragraph[lo + length] = (unsigned char)value;
    }
    /* Delete at a paragraph start inherits that paragraph, including empty EOF. */
    if (!lo) n->paragraph[0] = (unsigned char)(paragraphs && length ? paragraphs[0] : inherited);
    n->style[n->length] = (unsigned char)typing;
    if (writer_doc_validate(n)) { status("The edit could not be represented safely."); return 0; }
    accept_staging(lo + length, typing, group); return 1;
}
static int format_inline(unsigned mask) {
    unsigned lo = selection_low(), hi = selection_high(), typing = snapshot()->typing_style;
    int turn_on = 0;
    if (lo == hi) turn_on = !(typing & mask);
    else for (unsigned i = lo; i < hi; i++) if (!(document()->style[i] & mask)) { turn_on = 1; break; }
    Snapshot *s = begin_edit();
    for (unsigned i = lo; i < hi; i++) s->doc.style[i] = (unsigned char)(turn_on ? s->doc.style[i] | mask : s->doc.style[i] & ~mask);
    s->typing_style = turn_on ? typing | mask : typing & ~mask;
    s->doc.style[s->doc.length] = (unsigned char)s->typing_style;
    state.manual_scroll = 0; reveal();
    status("Character style changed."); return WRITER_CHANGED;
}
static int format_paragraph(unsigned mask, unsigned value) {
    unsigned lo = selection_low(), hi = selection_high();
    WriterDoc *d = document();
    unsigned first = paragraph_start(d, lo);
    unsigned last = paragraph_start(d, hi > lo ? hi - 1 : hi);
    Snapshot *s = begin_edit();
    for (unsigned p = first;;) {
        s->doc.paragraph[p] = (unsigned char)((s->doc.paragraph[p] & ~mask) | value);
        if (p >= last) break;
        p = paragraph_end(&s->doc, p) + 1;
    }
    state.manual_scroll = 0; reveal();
    status("Paragraph style changed."); return WRITER_CHANGED;
}
static void copy_selection(void) {
    state.group_kind = 0;
    unsigned lo = selection_low(), hi = selection_high();
    if (lo == hi) { status("Select text to copy."); return; }
    const WriterDoc *d = document(); WriterDoc *c = &ARENA->clipboard;
    c->length = hi - lo;
    for (unsigned i = 0; i < c->length; i++) {
        c->text[i] = d->text[lo + i]; c->style[i] = d->style[lo + i];
        c->paragraph[i] = i ? d->paragraph[lo + i] : (unsigned char)paragraph_at(d, lo + i);
    }
    c->style[c->length] = (unsigned char)snapshot()->typing_style;
    c->paragraph[c->length] = c->text[c->length - 1] == '\n' ? d->paragraph[hi] : 0;
    state.clipboard_length = c->length;
    state.clipboard_generation = writer_clipboard_set((const char *)c->text, c->length);
    state.clipboard_owned = 1;
    status("Copied. Styles stay with this Writer clipboard selection.");
}
static void paste(void) {
    unsigned generation = 0;
    int length = writer_clipboard_get((char *)ARENA->output, WRITER_TEXT_MAX + 1u, &generation);
    if (!length) { status("Clipboard is empty. Nothing was changed."); return; }
    state.group_kind = 0;
    const WriterDoc *c = &ARENA->clipboard;
    /* The zero-generation default indicates no shared clipboard integration. */
    int own = state.clipboard_owned && ((length < 0 && !state.clipboard_generation) ||
              (length >= 0 && generation == state.clipboard_generation && (unsigned)length == state.clipboard_length));
    if (own && length >= 0) for (int i = 0; i < length; i++) if (ARENA->output[i] != c->text[i]) { own = 0; break; }
    if (own) replace(selection_low(), selection_high(), c->text, c->length, c->style, c->paragraph, 0);
    else if (length >= 0 && (unsigned)length <= WRITER_TEXT_MAX) replace(selection_low(), selection_high(), ARENA->output, (unsigned)length, 0, 0, 0);
    else status("Clipboard is unavailable or exceeds the 32,768-byte limit.");
}

/* The standard UI glyphs are used at 1x (body) or 1.5x (heading). Italic is a
 * bounded synthetic right slant, bold an extra coverage pixel. Advance and
 * painting use the same dimensions so hit testing never assumes a monospace. */
static int scale_value(int n, unsigned paragraph) { return paragraph & WRITER_PARAGRAPH_HEADING ? (n * 3 + 1) / 2 : n; }
static int advance(unsigned char ch, unsigned style, unsigned paragraph, int x) {
    int space = scale_value(ui_advance(' '), paragraph);
    if (ch == '\t') { int stop = space * 4; return stop - x % stop; }
    int result = scale_value(ui_advance((char)ch), paragraph);
    if (style & WRITER_STYLE_BOLD) result++;
    if (style & WRITER_STYLE_ITALIC) result += 2;
    return result;
}
void writer_layout(int width) {
    if (!state.initialized) writer_init();
    width = clamp(width, 32, 4096);
    if (state.layout_valid && state.width == width && state.layout_revision == snapshot()->revision) return;
    const WriterDoc *d = document();
    unsigned at = 0, y = 0, paragraph = d->paragraph[0], count = 0;
    for (;;) {
        unsigned start = at, end = at, last_space = at;
        int used = 0, space_width = 0;
        while (end < d->length && d->text[end] != '\n') {
            int a = advance(d->text[end], d->style[end], paragraph, used);
            if (used + a > width && end > start) {
                if (last_space > start) { end = last_space; used = space_width; }
                break;
            }
            used += a; end++;
            if (d->text[end - 1] == ' ' || d->text[end - 1] == '\t') { last_space = end; space_width = used; }
        }
        Line *line = &ARENA->lines[count++];
        line->start = (unsigned short)start; line->end = (unsigned short)end;
        line->y = y; line->width = (unsigned short)used;
        line->height = paragraph & WRITER_PARAGRAPH_HEADING ? HEADING_HEIGHT : BODY_HEIGHT;
        line->paragraph = (unsigned char)paragraph;
        y += line->height;
        if (end == d->length) break;
        at = end;
        if (d->text[end] == '\n') { at++; paragraph = d->paragraph[at]; }
        if (!(count & 127u)) platform_poll();
    }
    state.line_count = count; state.width = width; state.content_height = (int)y;
    state.layout_revision = snapshot()->revision; state.layout_valid = 1;
    state.scroll = clamp(state.scroll, 0, state.content_height > state.viewport_height ? state.content_height - state.viewport_height : 0);
}
static int line_x(const Line *line) {
    int spare = state.width - line->width;
    if (spare < 0) spare = 0;
    unsigned align = line->paragraph & WRITER_ALIGN_MASK;
    return align == WRITER_ALIGN_CENTER ? spare / 2 : align == WRITER_ALIGN_RIGHT ? spare : 0;
}
static unsigned position_line(unsigned index) {
    unsigned low = 0, high = state.line_count;
    while (low + 1 < high) {
        unsigned mid = (low + high) / 2;
        if (ARENA->lines[mid].start <= index) low = mid; else high = mid;
    }
    return low;
}
static int index_x(const Line *line, unsigned index) {
    const WriterDoc *d = document(); int x = 0;
    for (unsigned p = line->start; p < index && p < line->end; p++) x += advance(d->text[p], d->style[p], line->paragraph, x);
    return x + line_x(line);
}
int writer_position(unsigned index, int *x, int *y, int *height) {
    writer_layout(state.width ? state.width : WRITER_W - 2 * PAGE_PAD - 18);
    if (index > document()->length) return 0;
    unsigned row = position_line(index);
    if (row && index == snapshot()->caret && snapshot()->affinity && ARENA->lines[row - 1].end == index) row--;
    const Line *line = &ARENA->lines[row];
    if (x) *x = index_x(line, index);
    if (y) *y = (int)line->y;
    if (height) *height = line->height;
    return 1;
}
static unsigned hit_line(unsigned row, int x) {
    const Line *line = &ARENA->lines[row]; const WriterDoc *d = document();
    int offset = line_x(line), used = 0;
    for (unsigned p = line->start; p < line->end; p++) {
        int a = advance(d->text[p], d->style[p], line->paragraph, used);
        if (x < offset + used + (a + 1) / 2) return p;
        used += a;
    }
    return line->end;
}
unsigned writer_hit_position(int x, int y) {
    writer_layout(state.width ? state.width : WRITER_W - 2 * PAGE_PAD - 18);
    unsigned low = 0, high = state.line_count;
    if (y < 0) y = 0;
    while (low + 1 < high) {
        unsigned mid = (low + high) / 2;
        if ((int)ARENA->lines[mid].y <= y) low = mid; else high = mid;
    }
    unsigned result = hit_line(low, x);
    state.hit_affinity = result == ARENA->lines[low].end && low + 1 < state.line_count && ARENA->lines[low + 1].start == result;
    return result;
}
static void reveal(void) {
    int x, y, height;
    writer_position(snapshot()->caret, &x, &y, &height);
    (void)x;
    if (y < state.scroll) state.scroll = y;
    if (y + height > state.scroll + state.viewport_height) state.scroll = y + height - state.viewport_height;
    state.scroll = clamp(state.scroll, 0, state.content_height > state.viewport_height ? state.content_height - state.viewport_height : 0);
}
static void move_to(unsigned index, int extend, int vertical) {
    state.group_kind = 0; state.manual_scroll = 0;
    Snapshot *s = snapshot();
    s->caret = min_u(index, s->doc.length); s->affinity = 0;
    if (!extend) s->anchor = s->caret;
    s->typing_style = s->caret < s->doc.length ? s->doc.style[s->caret] : s->doc.style[s->doc.length];
    if (!vertical) state.desired_x = -1;
    state.blink = 1; reveal();
}

static void move_hit(int x, int y, int extend, int vertical) {
    unsigned pos = writer_hit_position(x, y); int affinity = state.hit_affinity;
    move_to(pos, extend, vertical); snapshot()->affinity = (unsigned)affinity; reveal();
}

static unsigned string_length(const char *text) { unsigned n = 0; while (text[n]) n++; return n; }
static void append_number(char *text, unsigned capacity, unsigned number) {
    char digits[12]; unsigned count = 0, used = string_length(text);
    do { digits[count++] = (char)('0' + number % 10); number /= 10; } while (number);
    while (count && used + 1 < capacity) text[used++] = digits[--count];
    text[used] = 0;
}
unsigned writer_word_count(void) {
    if (!state.initialized) return 0;
    if (state.stats_revision != snapshot()->revision) {
        const WriterDoc *d = document(); int previous_word = 0;
        state.words = 0;
        for (unsigned i = 0; i < d->length; i++) {
            int word = d->text[i] != ' ' && d->text[i] != '\t' && d->text[i] != '\n';
            if (word && !previous_word) state.words++;
            previous_word = word;
            if (!(i & 4095u)) platform_poll();
        }
        state.stats_revision = snapshot()->revision;
    }
    return state.words;
}
static int search_height(void) { return state.search.open ? (state.search.replacing ? 92 : 62) : 0; }
static unsigned folded(unsigned c) { return !state.search.exact && c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int search_match(unsigned at) {
    const WriterDoc *d = document(); unsigned n = string_length(state.search.text[0]);
    if (!n || at > d->length || n > d->length - at) return 0;
    for (unsigned i = 0; i < n; i++) if (folded(d->text[at + i]) != folded((unsigned char)state.search.text[0][i])) return 0;
    return 1;
}
static void search_open(int replacing) {
    state.group_kind = 0;
    state.search.open = state.search.focus = 1; state.search.replacing = replacing; state.search.field = 0;
    unsigned lo = selection_low(), hi = selection_high();
    if (hi > lo && hi - lo < sizeof(state.search.text[0])) {
        int valid = 1;
        for (unsigned i = lo; i < hi; i++) if (document()->text[i] < 32) valid = 0;
        if (valid) {
            for (unsigned i = lo; i < hi; i++) state.search.text[0][i - lo] = (char)document()->text[i];
            state.search.text[0][hi - lo] = 0;
        }
    }
    state.search.anchor[0] = 0; state.search.caret[0] = string_length(state.search.text[0]);
    status(replacing ? "Find and replace: Enter finds; Ctrl+Enter replaces; Ctrl+Shift+Enter replaces all." : "Find: Enter/F3 next, Shift+Enter/F3 previous. Escape closes.");
}
static int search_find(int direction) {
    unsigned n = string_length(state.search.text[0]);
    if (!n) { status("Enter text to find."); return 0; }
    const WriterDoc *d = document();
    if (n > d->length) { status("Text not found."); return 0; }
    unsigned last = d->length - n;
    int start = direction < 0 ? (int)selection_low() - 1 : (int)snapshot()->caret;
    if (start < 0) start = (int)last;
    if ((unsigned)start > last) start = 0;
    unsigned at = (unsigned)start;
    do {
        if (search_match(at)) {
            move_to(at + n, 0, 0); snapshot()->anchor = at;
            status("Match selected."); return 1;
        }
        if (direction < 0) at = at ? at - 1 : last;
        else at = at < last ? at + 1 : 0;
        if (!(at & 511u)) platform_poll();
    } while (at != (unsigned)start);
    status("Text not found."); return 0;
}
static void search_replace_one(void) {
    unsigned n = string_length(state.search.text[0]), r = string_length(state.search.text[1]);
    if (!n) { status("Enter text to find before replacing."); return; }
    unsigned lo = selection_low(), hi = selection_high();
    if (hi - lo != n || !search_match(lo)) {
        if (!search_find(1)) return;
        lo = selection_low(); hi = selection_high();
    }
    for (unsigned i = 0; i < r; i++) ARENA->output[i] = document()->style[lo];
    if (replace(lo, hi, (const unsigned char *)state.search.text[1], r, ARENA->output, 0, 0)) {
        search_find(1); status("Replaced one match; following match selected when available.");
    }
}
static void search_replace_all(void) {
    unsigned q = string_length(state.search.text[0]), r = string_length(state.search.text[1]);
    if (!q) { status("Enter text to find before replacing."); return; }
    const WriterDoc *d = document(); unsigned matches = 0;
    for (unsigned i = 0; i < d->length;) {
        if (search_match(i)) { matches++; i += q; } else i++;
        if (!(i & 511u)) platform_poll();
    }
    if (!matches) { status("No matching text to replace."); return; }
    unsigned removed = matches * q, added = matches * r;
    if (added > WRITER_TEXT_MAX - (d->length - removed)) {
        status("Replacement exceeds 32,768 bytes. Nothing was changed."); return;
    }
    WriterDoc *out = &ARENA->staging; unsigned written = 0, para = d->paragraph[0];
    for (unsigned i = 0; i < d->length;) {
        if (is_start(d, i)) para = d->paragraph[i];
        if (search_match(i)) {
            for (unsigned j = 0; j < r; j++) {
                out->paragraph[written] = !written || out->text[written - 1] == '\n' ? (unsigned char)para : 0;
                out->text[written] = (unsigned char)state.search.text[1][j];
                out->style[written++] = d->style[i];
            }
            i += q;
        } else {
            out->paragraph[written] = !written || out->text[written - 1] == '\n' ? (unsigned char)para : 0;
            out->text[written] = d->text[i]; out->style[written++] = d->style[i++];
        }
        if (!(i & 511u)) platform_poll();
    }
    out->length = written; out->style[written] = d->style[d->length];
    out->paragraph[written] = !written ? d->paragraph[0] : out->text[written - 1] == '\n' ? d->paragraph[d->length] : 0;
    if (writer_doc_validate(out)) { status("Replacement could not be represented safely."); return; }
    accept_staging(0, snapshot()->typing_style, 0); reveal();
    char message[64] = ""; append_number(message, sizeof(message), matches);
    unsigned end = string_length(message); text_copy(message + end, sizeof(message) - end, " replacements. Undo restores all text and styles.");
    status(message);
}
static int search_field_replace(const unsigned char *inserted, unsigned n) {
    unsigned field = (unsigned)state.search.field, a = state.search.anchor[field], b = state.search.caret[field];
    unsigned lo = a < b ? a : b, hi = a > b ? a : b;
    char *text = state.search.text[field]; unsigned length = string_length(text);
    if (n > 63u - (length - (hi - lo))) { status("Search fields hold at most 63 ASCII characters. Nothing was changed."); return 0; }
    for (unsigned i = 0; i < n; i++) if (inserted[i] < 32 || inserted[i] > 126) {
        status("Search fields accept printable ASCII only. Nothing was changed."); return 0;
    }
    char next[64]; unsigned used = 0;
    for (unsigned i = 0; i < lo; i++) next[used++] = text[i];
    for (unsigned i = 0; i < n; i++) next[used++] = (char)inserted[i];
    for (unsigned i = hi; i < length; i++) next[used++] = text[i];
    next[used] = 0; text_copy(text, 64, next);
    state.search.caret[field] = state.search.anchor[field] = lo + n;
    return 1;
}
static int search_key(int sc, char ch, int modifiers) {
    unsigned f = (unsigned)state.search.field;
    char *text = state.search.text[f]; unsigned length = string_length(text), pos = state.search.caret[f];
    int control = modifiers & WRITER_MOD_CTRL, shift = modifiers & WRITER_MOD_SHIFT;
    if (sc == 0x0f) {
        state.search.field = state.search.replacing ? 1 - (int)f : 0;
        f = (unsigned)state.search.field; state.search.anchor[f] = 0; state.search.caret[f] = string_length(state.search.text[f]);
    } else if (sc == 0x1c) {
        if (control && state.search.replacing) { if (shift) search_replace_all(); else search_replace_one(); }
        else search_find(shift ? -1 : 1);
    } else if (control && sc == 0x1e) { state.search.anchor[f] = 0; state.search.caret[f] = length; }
    else if (control && (sc == 0x2e || sc == 0x2d)) {
        unsigned lo = min_u(pos, state.search.anchor[f]), hi = pos > state.search.anchor[f] ? pos : state.search.anchor[f];
        if (hi > lo) {
            writer_clipboard_set(text + lo, hi - lo); state.clipboard_owned = 0;
            if (sc == 0x2d) search_field_replace(0, 0);
        }
    } else if (control && sc == 0x2f) {
        unsigned generation;
        int n = writer_clipboard_get((char *)ARENA->output, WRITER_TEXT_MAX + 1u, &generation);
        if (n < 0) status("Clipboard is unavailable or too large for a search field.");
        else if (!n) status("Clipboard is empty. The search field is unchanged.");
        else search_field_replace(ARENA->output, (unsigned)n);
    } else if (sc == 0x4b || sc == 0x4d || sc == 0x47 || sc == 0x4f) {
        if (sc == 0x4b && pos) pos--; else if (sc == 0x4d && pos < length) pos++;
        else if (sc == 0x47) pos = 0; else if (sc == 0x4f) pos = length;
        state.search.caret[f] = pos; if (!shift) state.search.anchor[f] = pos;
    } else if (sc == 0x0e || sc == 0x53) {
        if (pos == state.search.anchor[f]) {
            if (sc == 0x0e && pos) state.search.anchor[f]--;
            else if (sc == 0x53 && pos < length) state.search.anchor[f]++;
        }
        search_field_replace(0, 0);
    } else if (!control && (unsigned char)ch >= 32 && (unsigned char)ch <= 126) {
        unsigned char byte = (unsigned char)ch; search_field_replace(&byte, 1);
    }
    return WRITER_CHANGED;
}

void writer_new(void) {
    state.pending_pdf.valid = 0;
    state.search.open = state.search.focus = 0;
    state.group_kind = 0;
    state.first = state.current = 0; state.count = 1;
    Snapshot *s = snapshot(); writer_doc_init(&s->doc);
    s->caret = s->anchor = s->typing_style = s->affinity = 0;
    s->revision = ++state.next_revision; state.saved_revision = s->revision;
    state.file = -1; state.identity = 0; state.has_binding = state.binding_conflict = 0; state.failed_save = state.dragging = 0;
    state.scroll = state.manual_scroll = 0; text_copy(state.title, sizeof(state.title), "Untitled");
    invalidate(); status("New document. Body text, printable ASCII and tabs.");
}
void writer_init(void) {
    if (state.initialized) return;
    state.initialized = 1; state.width = WRITER_W - 2 * PAGE_PAD - 18;
    state.viewport_height = WRITER_H - TOOL_H - FOOT_H - 16;
    writer_new();
}
static int extension(const char *name, const char *suffix) {
    unsigned n = 0; while (name[n]) n++;
    if (n < 4 || name[n - 4] != '.') return 0;
    for (unsigned i = 0; i < 3; i++) {
        unsigned c = (unsigned char)name[n - 3 + i];
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c != (unsigned char)suffix[i]) return 0;
    }
    return 1;
}
static int native_name(const char *name) {
    unsigned n = 0; while (name[n]) n++;
    return n >= 4 && name[n - 4] == '.' && (name[n - 3] == 'b' || name[n - 3] == 'B') &&
           (name[n - 2] == 'w' || name[n - 2] == 'W') && (name[n - 1] == 'r' || name[n - 1] == 'R');
}
static void fingerprint(const unsigned char *data, unsigned size, WriterBinding *out) {
    unsigned fnv = 2166136261u, crc = ~0u;
    for (unsigned i = 0; i < size; i++) {
        fnv = (fnv ^ data[i]) * 16777619u;
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        if (!(i & 2047u)) platform_poll();
    }
    out->size = size; out->hash_a = fnv; out->hash_b = ~crc;
}
int writer_binding_matches(int file, const WriterBinding *binding) {
    if (!binding || !fs_valid(file) || fs_is_dir(file) || fs_is_app(file) || !native_name(fs_name(file))) return 0;
    int size = fs_size(file);
    if (size < (int)WRITER_NATIVE_HEADER_SIZE + 2 || (unsigned)size > WRITER_NATIVE_MAX_SIZE || (unsigned)size != binding->size) return 0;
    const unsigned char *data = (const unsigned char *)fs_data(file);
    if (!data) return 0;
    WriterBinding actual; fingerprint(data, (unsigned)size, &actual);
    return actual.hash_a == binding->hash_a && actual.hash_b == binding->hash_b;
}
int writer_open_file(int id) {
    state.group_kind = 0;
    if (!state.initialized) writer_init();
    if (!fs_valid(id) || fs_is_dir(id) || fs_is_app(id)) { status("That file is unavailable."); return 0; }
    int size = fs_size(id); const unsigned char *data = (const unsigned char *)fs_data(id);
    int native = native_name(fs_name(id));
    if (extension(fs_name(id), "pdf") || (size >= 5 && data && data[0] == '%' && data[1] == 'P' && data[2] == 'D' && data[3] == 'F' && data[4] == '-')) {
        status("PDF is export-only. Open the original .bwr to edit; view or print PDF on another computer."); return 0;
    }
    if (extension(fs_name(id), "rtf") || (size >= 5 && data && data[0] == '{' && data[1] == '\\' && data[2] == 'r' && data[3] == 't' && data[4] == 'f')) {
        status("RTF is export-only. Open the original .bwr or import a plain-text copy."); return 0;
    }
    if (size < 0 || !data || (native ? writer_native_decode(&ARENA->staging, data, (unsigned)size) :
                             writer_plain_import(&ARENA->staging, data, (unsigned)size))) {
        status(native ? "Invalid or unsupported .bwr document; current work is unchanged." :
                        "Import rejected: ASCII with LF/CRLF breaks, at most 32,768 normalized bytes."); return 0;
    }
    state.pending_pdf.valid = 0;
    state.search.open = state.search.focus = 0;
    state.first = state.current = 0; state.count = 1;
    Snapshot *s = snapshot(); copy_bytes(&s->doc, &ARENA->staging, sizeof(WriterDoc));
    s->caret = s->anchor = s->affinity = 0; s->typing_style = s->doc.style[0];
    s->revision = ++state.next_revision; state.saved_revision = native ? s->revision : 0;
    state.file = native ? id : -1; state.identity = native ? fs_identity(id) : 0;
    state.has_binding = native; state.binding_conflict = 0;
    if (native) fingerprint(data, (unsigned)size, &state.binding);
    state.failed_save = state.dragging = 0; state.scroll = state.manual_scroll = 0;
    text_copy(state.title, sizeof(state.title), fs_name(id));
    invalidate(); status(native ? "Opened native document." : "Imported text. Save As a new .bwr to retain formatting."); return 1;
}
static int binding_valid(void) {
    return state.file >= 0 && fs_valid(state.file) && !fs_is_dir(state.file) && !fs_is_app(state.file) &&
           native_name(fs_name(state.file)) && fs_identity(state.file) == state.identity;
}
int writer_binding(WriterBinding *out) {
    if (!out || !state.initialized || !state.has_binding || !binding_valid()) return 0;
    *out = state.binding; return 1;
}
const unsigned char *writer_snapshot(unsigned *length) {
    if (!state.initialized) writer_init();
    if (writer_native_encode(document(), ARENA->output, EXPORT_CAPACITY, length)) { status("Could not encode document."); return 0; }
    return ARENA->output;
}
static int save_to(int id) {
    unsigned length;
    if (!writer_snapshot(&length)) return WRITER_SAVE_ERROR;
    int written = fs_write(id, (const char *)ARENA->output, (int)length);
    if (written == FS_ERR_BUSY) { status("Disk is saving; retry shortly."); return WRITER_SAVE_ERROR; }
    if (written < 0) {
        state.failed_save = 1; status("Save failed; the complete document remains open."); return WRITER_SAVE_ERROR;
    }
    /* Update the baseline immediately after the atomic RAM write, including
     * failed-sync retries. This target is ours, even for a new Save As. */
    state.file = id; state.identity = fs_identity(id); state.has_binding = 1; state.binding_conflict = 0;
    fingerprint(ARENA->output, length, &state.binding);
    text_copy(state.title, sizeof(state.title), fs_name(id));
    /* fs_write may replace bytes in RAM; only fs_sync makes this a saved state. */
    if (fs_sync() < 0) {
        state.failed_save = 1; status("Disk sync failed. Unsaved work remains open; retry Save."); return WRITER_SAVE_ERROR;
    }
    state.file = id; state.identity = fs_identity(id); state.saved_revision = snapshot()->revision;
    state.failed_save = 0; text_copy(state.title, sizeof(state.title), fs_name(id));
    status("Saved and synchronized to disk."); return WRITER_SAVE_OK;
}
int writer_save(void) {
    if (fs_sync_busy()) { status("Disk is saving; retry shortly."); return WRITER_SAVE_ERROR; }
    state.group_kind = 0;
    if (!state.initialized) writer_init();
    if (!binding_valid()) { status("Choose Save As for a new native .bwr file."); return WRITER_SAVE_NEEDS_NAME; }
    if (!state.has_binding || !writer_binding_matches(state.file, &state.binding)) {
        state.binding_conflict = 1;
        status("The source file changed outside Writer. Save As a new .bwr; the source is unchanged.");
        return WRITER_SAVE_NEEDS_NAME;
    }
    return save_to(state.file);
}
int writer_save_as(int parent, const char *name) {
    if (fs_sync_busy()) { status("Disk is saving; retry shortly."); return WRITER_SAVE_ERROR; }
    state.group_kind = 0;
    if (!state.initialized) writer_init();
    if (!name || !native_name(name)) { status("Native documents use the .bwr filename extension."); return WRITER_SAVE_ERROR; }
    int existing = fs_find_child(parent, name);
    if (existing >= 0) {
        if (binding_valid() && existing == state.file) return writer_save();
        status("That name already exists. Choose a new name; no file was replaced."); return WRITER_SAVE_ERROR;
    }
    unsigned length;
    if (!writer_snapshot(&length)) return WRITER_SAVE_ERROR;
    if (length > fs_file_limit()) { status("Native document exceeds this volume's per-file limit; no file was created."); return WRITER_SAVE_ERROR; }
    int id = fs_create(parent, name);
    if (id < 0) { status("Could not create that file; check name, space and disk status."); return WRITER_SAVE_ERROR; }
    unsigned identity = fs_identity(id);
    int result = save_to(id);
    if (result != WRITER_SAVE_OK) {
        /* A created target with a pending verified-write failure is ours, never
         * an unrelated reused node. Keep its identity so a retry is safe. */
        if (fs_valid(id) && fs_identity(id) == identity && !fs_size(id)) {
            fs_delete(id);
        } else if (fs_valid(id) && fs_identity(id) == identity) {
            state.file = id; state.identity = identity;
            text_copy(state.title, sizeof(state.title), name);
        }
    }
    return result;
}
int writer_export_rtf(int parent, const char *name) {
    if (fs_sync_busy()) { status("Disk is saving; retry shortly."); return -1; }
    state.group_kind = 0;
    if (!state.initialized) writer_init();
    if (!name || !extension(name, "rtf")) { status("RTF exports use the .rtf filename extension."); return -1; }
    if (fs_find_child(parent, name) >= 0) {
        status("Choose a new export filename; existing files are never replaced."); return -1;
    }
    unsigned length;
    if (writer_rtf_export(document(), ARENA->output, EXPORT_CAPACITY, &length)) {
        status("RTF export exceeds its 512 KiB bound; simplify styles or split the document."); return -1;
    }
    if (length > fs_file_limit()) { status("RTF exceeds this volume's per-file limit; no export was created."); return -1; }
    int id = fs_create(parent, name);
    if (id < 0) { status("Could not create the export file."); return -1; }
    if (fs_write(id, (const char *)ARENA->output, (int)length) < 0) {
        fs_delete(id); status("RTF write failed. Native document is unchanged."); return -1;
    }
    if (fs_sync() < 0) { status("RTF exists in memory, but disk sync failed; native work is unchanged."); return -1; }
    status("Exported RTF and synchronized to disk. Native save state is unchanged."); return id;
}
int writer_export_pdf(int parent, const char *name, unsigned paper) {
    if (fs_sync_busy()) { status("Disk is saving; retry shortly."); return -1; }
    if (!state.initialized) writer_init();
    unsigned n = 0;
    if (name) while (n < FS_NAME_LEN && name[n] && name[n] != '/') n++;
    if (!name || !n || n == FS_NAME_LEN || name[n]) {
        status("Choose a PDF name of 1-23 characters, with no slash."); return -1;
    }
    if (!extension(name, "pdf")) { status("PDF exports use the .pdf filename extension."); return -1; }
    if (!fs_is_dir(parent)) { status("The export folder is unavailable; choose a valid folder."); return -1; }
    if (paper != WRITER_PDF_LETTER && paper != WRITER_PDF_A4) {
        status("Choose Letter or A4 paper; no PDF was created."); return -1;
    }
    int existing = fs_find_child(parent, name);
    if (existing >= 0 && (!state.pending_pdf.valid || existing != state.pending_pdf.file ||
        fs_identity(existing) != state.pending_pdf.identity || fs_is_dir(existing) || fs_is_app(existing))) {
        status("Choose a new PDF name; existing files are never replaced."); return -1;
    }
    if (existing >= 0 && (snapshot()->revision != state.pending_pdf.revision || paper != state.pending_pdf.paper)) {
        status("Pending PDF differs from this document or paper. Choose a new name; the old PDF is unchanged."); return -1;
    }
    unsigned length, pages;
    if (writer_pdf_export(document(), paper, 0, 0, &length, &pages) != WRITER_PDF_OK) {
        status("Could not measure PDF; the native document is unchanged."); return -1;
    }
    if (length > EXPORT_CAPACITY) {
        status("PDF exceeds the 512 KiB export buffer. Simplify styles or split the document; no file was created."); return -1;
    }
    if (length > fs_file_limit()) { status("PDF exceeds this volume's per-file limit; no export was created."); return -1; }
    if (existing < 0) {
        unsigned count = (unsigned)fs_node_count(), capacity = fs_capacity_for_nodes(count + 1), used = fs_used_bytes();
        if (count >= (unsigned)fs_node_limit()) { status("The volume has no free file slots; no PDF was created."); return -1; }
        if (used > capacity || length > capacity - used) {
            status("Not enough space for the complete PDF; no export was created."); return -1;
        }
    }
    unsigned written, actual_pages;
    if (writer_pdf_export(document(), paper, ARENA->output, EXPORT_CAPACITY, &written, &actual_pages) != WRITER_PDF_OK ||
        written != length || actual_pages != pages) {
        status("Could not serialize the complete PDF; no export was created."); return -1;
    }
    int id = existing;
    if (id >= 0) {
        /* A retry is sync-only, never an overwrite. Revision/paper guarantee the
         * same deterministic export; compare every byte to detect outside edits.
         * Polls only service devices and cannot dispatch edits or file changes. */
        const unsigned char *bytes = (const unsigned char *)fs_data(id);
        if (!bytes || fs_size(id) != (int)length || length != state.pending_pdf.length) {
            status("The pending PDF changed outside Writer. Choose a new name; no file was replaced."); return -1;
        }
        for (unsigned i = 0; i < length; i++) {
            if (bytes[i] != ARENA->output[i]) {
                status("The pending PDF changed outside Writer. Choose a new name; no file was replaced."); return -1;
            }
            if ((i & 4095u) == 4095u) platform_poll();
        }
    } else {
        id = fs_create(parent, name);
        if (id < 0) { status("Could not create the PDF; check filename, folder, space and disk status."); return -1; }
        unsigned identity = fs_identity(id);
        if (fs_write(id, (const char *)ARENA->output, (int)length) < 0) {
            /* Only the new empty target is ours to roll back. */
            if (fs_valid(id) && fs_identity(id) == identity && !fs_is_dir(id) && !fs_is_app(id) && !fs_size(id)) fs_delete(id);
            status("PDF write failed. Native work is unchanged; no complete export was written."); return -1;
        }
        state.pending_pdf.valid = 1; state.pending_pdf.file = id; state.pending_pdf.identity = identity;
        state.pending_pdf.revision = snapshot()->revision; state.pending_pdf.paper = paper; state.pending_pdf.length = length;
    }
    if (fs_sync() < 0) {
        status("PDF exists in RAM; disk sync failed. Retry this name and paper. Native work is unchanged."); return -1;
    }
    state.pending_pdf.valid = 0;
    status("Exported PDF and synchronized to disk. Native save state is unchanged."); return id;
}
int writer_restore(const unsigned char *data, unsigned length, int file, unsigned identity,
                   int dirty, unsigned caret, unsigned anchor) {
    state.group_kind = 0;
    if (!state.initialized) writer_init();
    if (writer_native_decode(&ARENA->staging, data, length)) { status("Invalid recovery document; current work is unchanged."); return 0; }
    state.pending_pdf.valid = 0;
    state.search.open = state.search.focus = 0;
    state.first = state.current = 0; state.count = 1;
    Snapshot *s = snapshot(); copy_bytes(&s->doc, &ARENA->staging, sizeof(WriterDoc));
    s->affinity = 0; s->caret = min_u(caret, s->doc.length); s->anchor = min_u(anchor, s->doc.length);
    s->typing_style = s->doc.style[s->caret]; s->revision = ++state.next_revision;
    state.file = file; state.identity = identity;
    state.has_binding = state.binding_conflict = 0;
    if (!binding_valid()) { state.file = -1; state.identity = 0; dirty = 1; }
    else {
        int size = fs_size(state.file);
        if (size < (int)WRITER_NATIVE_HEADER_SIZE + 2 || (unsigned)size > WRITER_NATIVE_MAX_SIZE) {
            state.file = -1; state.identity = 0; dirty = 1;
        } else {
            fingerprint((const unsigned char *)fs_data(state.file), (unsigned)size, &state.binding);
            state.has_binding = 1;
        }
    }
    state.saved_revision = dirty ? 0 : s->revision; state.failed_save = 0;
    text_copy(state.title, sizeof(state.title), state.file >= 0 ? fs_name(state.file) : "Recovered document");
    state.scroll = state.dragging = state.manual_scroll = 0; invalidate(); status("Recovered document draft."); return 1;
}
const char *writer_title(void) { return state.initialized ? state.title : "Untitled"; }
const char *writer_status(void) { return state.status; }
int writer_dirty(void) { return state.initialized && (state.failed_save || state.binding_conflict || snapshot()->revision != state.saved_revision || (state.file >= 0 && !binding_valid())); }
int writer_read_only(void) { return 0; } /* All valid imports are editable copies. */
int writer_file(void) { return state.initialized && binding_valid() ? state.file : -1; }
unsigned writer_file_identity(void) { return state.initialized && binding_valid() ? state.identity : 0; }
unsigned writer_length(void) { return state.initialized ? document()->length : 0; }
unsigned writer_caret(void) { return state.initialized ? snapshot()->caret : 0; }
unsigned writer_anchor(void) { return state.initialized ? snapshot()->anchor : 0; }
const WriterDoc *writer_document(void) { if (!state.initialized) writer_init(); return document(); }
unsigned writer_line_count(void) { writer_layout(state.width); return state.line_count; }
void writer_close(void) { if (state.initialized) writer_new(); }
void writer_release(void) { if (state.initialized) state.dragging = 0; }
int writer_scroll(int lines) {
    if (lines) state.manual_scroll = 1;
    writer_layout(state.width);
    int old = state.scroll;
    state.scroll = clamp(state.scroll + lines * BODY_HEIGHT, 0,
                         state.content_height > state.viewport_height ? state.content_height - state.viewport_height : 0);
    return old != state.scroll;
}
int writer_tick(void) {
    if (!state.initialized) return 0;
    int blink = (int)((timer_ticks() / 35u) & 1u);
    if (blink != state.last_blink) { state.last_blink = blink; state.blink = !blink; return WRITER_CHANGED; }
    return 0;
}
int writer_key(int sc, char ch, int modifiers) {
    if (!state.initialized) writer_init();
    int control = modifiers & WRITER_MOD_CTRL, extend = modifiers & WRITER_MOD_SHIFT;
    if (modifiers & WRITER_MOD_ALT) return 0;
    if (control && (sc == 0x21 || sc == 0x23)) { search_open(sc == 0x23); return WRITER_CHANGED; }
    if (sc == 0x3d) { if (!state.search.open) search_open(0); search_find(extend ? -1 : 1); return WRITER_CHANGED; }
    if (state.search.open && sc == 0x01) { state.search.open = state.search.focus = 0; return WRITER_CHANGED; }
    if (state.search.open && state.search.focus && !(control && (sc == 0x1f || sc == 0x2c || sc == 0x15 || ((sc == 0x12 || sc == 0x19) && extend))))
        return search_key(sc, ch, modifiers);
    if (control) {
        switch (sc) {
        case 0x1f: state.group_kind = 0; return extend ? WRITER_REQUEST_SAVE_AS : WRITER_REQUEST_SAVE;
        case 0x12: if (extend) { state.group_kind = 0; return WRITER_REQUEST_EXPORT; } return format_paragraph(WRITER_ALIGN_MASK, WRITER_ALIGN_CENTER);
        case 0x19: if (extend) { state.group_kind = 0; return WRITER_REQUEST_PDF; } return 0;
        case 0x30: return format_inline(WRITER_STYLE_BOLD);
        case 0x17: return format_inline(WRITER_STYLE_ITALIC);
        case 0x16: return format_inline(WRITER_STYLE_UNDERLINE);
        case 0x02: return format_paragraph(WRITER_PARAGRAPH_HEADING, WRITER_PARAGRAPH_HEADING);
        case 0x0b: return format_paragraph(WRITER_PARAGRAPH_HEADING, 0);
        case 0x26: return format_paragraph(WRITER_ALIGN_MASK, WRITER_ALIGN_LEFT);
        case 0x13: return format_paragraph(WRITER_ALIGN_MASK, WRITER_ALIGN_RIGHT);
        case 0x2c: undo(extend); reveal(); return WRITER_CHANGED;
        case 0x15: undo(1); reveal(); return WRITER_CHANGED;
        case 0x1e: snapshot()->anchor = 0; move_to(document()->length, 1, 0); return WRITER_CHANGED;
        case 0x2e: copy_selection(); return WRITER_CHANGED;
        case 0x2d: copy_selection(); if (selection_low() != selection_high()) replace(selection_low(), selection_high(), 0, 0, 0, 0, 0); reveal(); return WRITER_CHANGED;
        case 0x2f: paste(); reveal(); return WRITER_CHANGED;
        }
    }
    unsigned pos = snapshot()->caret, low = selection_low(), high = selection_high();
    writer_layout(state.width);
    unsigned row = position_line(pos);
    if (row && snapshot()->affinity && ARENA->lines[row - 1].end == pos) row--;
    const Line *line = &ARENA->lines[row];
    if (sc == 0x4b || sc == 0x4d) {
        if (!extend && low != high) pos = sc == 0x4b ? low : high;
        else if (sc == 0x4b && pos) {
            pos--;
            if (control) {
                while (pos && (document()->text[pos] == ' ' || document()->text[pos] == '\t' || document()->text[pos] == '\n')) pos--;
                while (pos && document()->text[pos - 1] != ' ' && document()->text[pos - 1] != '\t' && document()->text[pos - 1] != '\n') pos--;
            }
        } else if (sc == 0x4d && pos < document()->length) {
            pos++;
            if (control) {
                while (pos < document()->length && document()->text[pos] != ' ' && document()->text[pos] != '\t' && document()->text[pos] != '\n') pos++;
                while (pos < document()->length && (document()->text[pos] == ' ' || document()->text[pos] == '\t' || document()->text[pos] == '\n')) pos++;
            }
        }
        move_to(pos, extend, 0); return WRITER_CHANGED;
    }
    if (sc == 0x47 || sc == 0x4f) {
        pos = control ? (sc == 0x47 ? 0 : document()->length) : (sc == 0x47 ? line->start : line->end);
        int upstream = !control && sc == 0x4f && row + 1 < state.line_count && ARENA->lines[row + 1].start == pos;
        move_to(pos, extend, 0); snapshot()->affinity = (unsigned)upstream; reveal(); return WRITER_CHANGED;
    }
    if (sc == 0x48 || sc == 0x50 || sc == 0x49 || sc == 0x51) {
        int x, y, height; writer_position(pos, &x, &y, &height);
        if (state.desired_x < 0) state.desired_x = x;
        if (sc == 0x48) y = (int)ARENA->lines[row ? row - 1 : 0].y;
        else if (sc == 0x50) y = (int)ARENA->lines[row + 1 < state.line_count ? row + 1 : row].y;
        else if (sc == 0x49) y -= state.viewport_height;
        else y += state.viewport_height;
        move_hit(state.desired_x, y, extend, 1); return WRITER_CHANGED;
    }
    if (control) return 0;
    if (sc == 0x0e || sc == 0x53) {
        unsigned group = low == high ? (sc == 0x0e ? 2u : 3u) : 0;
        if (low == high) {
            if (sc == 0x0e && low) low--;
            if (sc == 0x53 && high < document()->length) high++;
        }
        if (low != high) {
            if (high - low != 1 || document()->text[low] == '\n') group = 0;
            replace(low, high, 0, 0, 0, 0, group);
        }
        reveal(); return WRITER_CHANGED;
    }
    unsigned char inserted = (unsigned char)ch;
    if (sc == 0x1c) inserted = '\n';
    else if (sc == 0x0f) inserted = '\t';
    else if (inserted < 32 || inserted > 126) return 0;
    replace(low, high, &inserted, 1, 0, 0, low == high && inserted >= 32 ? 1u : 0); reveal(); return WRITER_CHANGED;
}

/* Local clipping is mandatory: gfx primitives otherwise clip only to screen. */
typedef struct { int x, y, w, h; } Clip;
static void rect(Clip c, int x, int y, int w, int h, unsigned char color) {
    int right = x + w, bottom = y + h;
    if (x < c.x) x = c.x;
    if (y < c.y) y = c.y;
    if (right > c.x + c.w) right = c.x + c.w;
    if (bottom > c.y + c.h) bottom = c.y + c.h;
    if (right > x && bottom > y) draw_rect(x, y, right - x, bottom - y, color);
}
static void glyph(Clip clip, unsigned char ch, int x, int y, unsigned style, unsigned paragraph, unsigned char color) {
    int bpr, h, w; const unsigned char *bits = ui_glyph((char)ch, &bpr, &h, &w);
    if (!bits || ch == ' ' || ch == '\t') return;
    int dh = scale_value(h, paragraph), dw = scale_value(w, paragraph);
    for (int yy = 0; yy < dh; yy++) {
        int heading = !!(paragraph & WRITER_PARAGRAPH_HEADING);
        int sy = heading ? yy * 2 / 3 : yy;
        int wy = heading && (yy * 2) % 3 == 2 ? 1 : 2;
        int slant = style & WRITER_STYLE_ITALIC ? (dh - 1 - yy) / 6 : 0;
        for (int xx = 0; xx < dw; xx++) {
            int sx = heading ? xx * 2 / 3 : xx;
            int level = glyph_level(bits, bpr, sy, sx);
            if (heading) {
                /* Exact box coverage for 3:2 scaling: source cells span three
                 * units, destination cells two. At most four source cells
                 * contribute; integer weights total four, preserving edges. */
                int wx = (xx * 2) % 3 == 2 ? 1 : 2;
                int right = wx == 1 && sx + 1 < w ? glyph_level(bits, bpr, sy, sx + 1) : level;
                int below = wy == 1 && sy + 1 < h ? glyph_level(bits, bpr, sy + 1, sx) : level;
                int diagonal = wx == 1 && wy == 1 && sx + 1 < w && sy + 1 < h ? glyph_level(bits, bpr, sy + 1, sx + 1) : level;
                level = (level * wx * wy + right * (2 - wx) * wy + below * wx * (2 - wy) + diagonal * (2 - wx) * (2 - wy) + 2) / 4;
            }
            if (!level) continue;
            int px = x + xx + slant, py = y + yy;
            if (px >= clip.x && px < clip.x + clip.w && py >= clip.y && py < clip.y + clip.h) {
                unsigned char mixed = level >= 15 ? color : gfx_mix(get_pixel(px, py), color, (level * 256 + 7) / 15);
                put_pixel(px, py, mixed);
            }
            if (style & WRITER_STYLE_BOLD) {
                px++;
                if (px >= clip.x && px < clip.x + clip.w && py >= clip.y && py < clip.y + clip.h) {
                    unsigned char mixed = level >= 15 ? color : gfx_mix(get_pixel(px, py), color, (level * 256 + 7) / 15);
                    put_pixel(px, py, mixed);
                }
            }
        }
    }
}
static void label(Clip c, const char *text, int x, int y, unsigned char color) {
    while (*text) { glyph(c, (unsigned char)*text, x, y, 0, 0, color); x += ui_advance(*text++); if (x >= c.x + c.w) break; }
}

typedef struct { short x, w; const char *label; } SearchButton;
static const SearchButton search_buttons[] = {
    {8, 48, "Prev"}, {60, 48, "Next"}, {112, 36, "Aa"},
    {152, 62, "Replace"}, {218, 42, "All"}, {264, 52, "Close"}
};
static int search_offset(unsigned field, int available) {
    int prefix = 0;
    for (unsigned i = 0; i < state.search.caret[field]; i++) prefix += ui_advance(state.search.text[field][i]);
    return prefix >= available ? prefix - available + 8 : 0;
}
static void draw_search(Clip all) {
    if (!state.search.open) return;
    int top = all.y + all.h - FOOT_H - search_height();
    rect(all, all.x, top, all.w, search_height(), gfx_rgb(237, 241, 247));
    unsigned rows = state.search.replacing ? 2 : 1;
    for (unsigned f = 0; f < rows; f++) {
        int y = top + 4 + (int)f * 30;
        label(all, f ? "With" : "Find", all.x + 8, y + 3, COLOR_DKGRAY);
        rect(all, all.x + 52, y, all.w - 60, 25, state.search.focus && (unsigned)state.search.field == f ? COLOR_BLUE : COLOR_GRAY);
        rect(all, all.x + 53, y + 1, all.w - 62, 23, COLOR_WHITE);
        Clip field = {all.x + 57, y + 3, all.w - 70, 19};
        if (field.x < all.x) field.x = all.x;
        if (field.x + field.w > all.x + all.w) field.w = all.x + all.w - field.x;
        if (field.y < all.y) field.y = all.y;
        if (field.y + field.h > all.y + all.h) field.h = all.y + all.h - field.y;
        int x = field.x - search_offset(f, field.w);
        unsigned a = state.search.anchor[f], b = state.search.caret[f];
        unsigned lo = a < b ? a : b, hi = a > b ? a : b;
        const char *text = state.search.text[f];
        for (unsigned i = 0; text[i]; i++) {
            int selected = state.search.focus && (unsigned)state.search.field == f && i >= lo && i < hi;
            int width = ui_advance(text[i]);
            if (selected) rect(field, x, field.y, width, field.h, COLOR_BLUE);
            glyph(field, (unsigned char)text[i], x, field.y, 0, 0, selected ? COLOR_WHITE : COLOR_DKGRAY);
            x += width;
        }
        if (state.search.focus && (unsigned)state.search.field == f && lo == hi && state.blink) {
            int caret_x = field.x - search_offset(f, field.w);
            for (unsigned i = 0; i < b; i++) caret_x += ui_advance(text[i]);
            rect(field, caret_x, field.y, 1, field.h, COLOR_BLACK);
        }
    }
    int y = top + 4 + (int)rows * 30;
    for (unsigned i = 0; i < sizeof search_buttons / sizeof search_buttons[0]; i++) {
        if ((i == 3 || i == 4) && !state.search.replacing) continue;
        const SearchButton *b = &search_buttons[i]; int selected = i == 2 && state.search.exact;
        rect(all, all.x + b->x, y, b->w, 24, selected ? COLOR_BLUE : COLOR_WHITE);
        label(all, b->label, all.x + b->x + (b->w - ui_string_w(b->label)) / 2, y + 3, selected ? COLOR_WHITE : COLOR_DKGRAY);
    }
}
static int search_click(int x, int y, int w, int h, int mx, int my, int modifiers) {
    if (!state.search.open) return 0;
    int top = y + h - FOOT_H - search_height();
    if (my < top || my >= y + h - FOOT_H) return 0;
    unsigned rows = state.search.replacing ? 2 : 1;
    state.group_kind = 0; state.dragging = 0;
    for (unsigned f = 0; f < rows; f++) {
        int fy = top + 4 + (int)f * 30;
        if (mx >= x + 52 && mx < x + w - 8 && my >= fy && my < fy + 25) {
            int local = mx - x - 57 + search_offset(f, w - 70), used = 0;
            unsigned pos = 0; const char *text = state.search.text[f];
            while (text[pos]) {
                int width = ui_advance(text[pos]); if (local < used + (width + 1) / 2) break;
                used += width; pos++;
            }
            state.search.focus = 1; state.search.field = (int)f; state.search.caret[f] = pos;
            if (!(modifiers & WRITER_MOD_SHIFT)) state.search.anchor[f] = pos;
            return WRITER_CHANGED;
        }
    }
    int by = top + 4 + (int)rows * 30;
    for (unsigned i = 0; i < sizeof search_buttons / sizeof search_buttons[0]; i++) {
        if ((i == 3 || i == 4) && !state.search.replacing) continue;
        const SearchButton *b = &search_buttons[i];
        if (mx >= x + b->x && mx < x + b->x + b->w && my >= by && my < by + 24) {
            if (i == 0 || i == 1) search_find(i ? 1 : -1);
            else if (i == 2) { state.search.exact = !state.search.exact; status(state.search.exact ? "Search is case-sensitive." : "Search ignores ASCII letter case."); }
            else if (i == 3) search_replace_one(); else if (i == 4) search_replace_all();
            else state.search.open = state.search.focus = 0;
            return WRITER_CHANGED;
        }
    }
    return WRITER_CHANGED;
}
typedef struct { short x, y, w; const char *name; unsigned action; } Button;
/* Both drawing and hit testing consume this same bounded toolbar geometry. */
static const Button buttons[] = {
    {8, 5, 28, "B", 1}, {40, 5, 28, "I", 2}, {72, 5, 28, "U", 3},
    {108, 5, 54, "Body", 4}, {166, 5, 76, "Heading", 5},
    {252, 5, 54, "Undo", 9}, {310, 5, 54, "Redo", 10},
    {8, 35, 54, "Left", 6}, {66, 35, 68, "Center", 7}, {138, 35, 60, "Right", 8},
    {208, 35, 58, "Save", 11}, {270, 35, 42, "RTF", 12}, {316, 35, 42, "PDF", 14}, {366, 35, 46, "Find", 13}
};
static int button_active(unsigned action) {
    unsigned style = snapshot()->typing_style, para = paragraph_at(document(), snapshot()->caret);
    if (action <= 3) return !!(style & (1u << (action - 1)));
    if (action == 4 || action == 5) return !!(para & WRITER_PARAGRAPH_HEADING) == (action == 5);
    if (action >= 6 && action <= 8) return (para & WRITER_ALIGN_MASK) == action - 6;
    return 0;
}
static int button_action(unsigned action) {
    state.group_kind = 0;
    if (action <= 3) return format_inline(1u << (action - 1));
    if (action == 4 || action == 5) return format_paragraph(WRITER_PARAGRAPH_HEADING, action == 5 ? WRITER_PARAGRAPH_HEADING : 0);
    if (action >= 6 && action <= 8) return format_paragraph(WRITER_ALIGN_MASK, action - 6);
    if (action == 9 || action == 10) return undo(action == 10);
    if (action == 11) return WRITER_REQUEST_SAVE;
    if (action == 12) return WRITER_REQUEST_EXPORT;
    if (action == 14) return WRITER_REQUEST_PDF;
    search_open(0); return WRITER_CHANGED;
}
static void geometry(int w, int h) {
    int old_width = state.width, old_height = state.viewport_height;
    state.viewport_height = h - TOOL_H - FOOT_H - 16 - search_height();
    if (state.viewport_height < 24) state.viewport_height = 24;
    writer_layout(w - 2 * PAGE_PAD - 18);
    if (!state.manual_scroll && (old_width != state.width || old_height != state.viewport_height)) reveal();
}
void writer_draw(int x, int y, int w, int h) {
    if (!state.initialized) writer_init();
    if (w <= 0 || h <= 0) return;
    geometry(w, h);
    Clip all = {x, y, w, h};
    rect(all, x, y, w, h, COLOR_LTGRAY);
    rect(all, x, y, w, TOOL_H, gfx_rgb(237, 241, 247));
    for (unsigned i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
        const Button *b = &buttons[i];
        int active = button_active(b->action);
        rect(all, x + b->x, y + b->y, b->w, 25, active ? COLOR_BLUE : COLOR_WHITE);
        label(all, b->name, x + b->x + (b->w - ui_string_w(b->name)) / 2, y + b->y + 3, active ? COLOR_WHITE : COLOR_DKGRAY);
    }
    Clip page = {x + 8, y + TOOL_H + 8, w - 24, h - TOOL_H - FOOT_H - 16 - search_height()};
    rect(all, page.x, page.y, page.w, page.h, COLOR_WHITE);
    Clip content = {x + PAGE_PAD, page.y, state.width + 4, page.h};
    if (content.x + content.w > page.x + page.w) content.w = page.x + page.w - content.x;
    const WriterDoc *d = document(); unsigned lo = selection_low(), hi = selection_high();
    unsigned start = 0;
    while (start + 1 < state.line_count && (int)(ARENA->lines[start].y + ARENA->lines[start].height) <= state.scroll) start++;
    for (unsigned row = start; row < state.line_count; row++) {
        const Line *line = &ARENA->lines[row];
        int py = content.y + (int)line->y - state.scroll;
        if (py >= content.y + content.h) break;
        int base = content.x + line_x(line), used = 0;
        for (unsigned p = line->start; p < line->end; p++) {
            int a = advance(d->text[p], d->style[p], line->paragraph, used);
            int selected = p >= lo && p < hi;
            if (selected) rect(content, base + used, py, a, line->height, COLOR_BLUE);
            glyph(content, d->text[p], base + used, py + 2, d->style[p], line->paragraph, selected ? COLOR_WHITE : COLOR_DKGRAY);
            if (d->style[p] & WRITER_STYLE_UNDERLINE)
                rect(content, base + used, py + scale_value(UI_HEIGHT, line->paragraph), a, 1, selected ? COLOR_WHITE : COLOR_DKGRAY);
            used += a;
        }
        if (line->end < d->length && d->text[line->end] == '\n' && line->end >= lo && line->end < hi)
            rect(content, base + used, py, 5, line->height, COLOR_BLUE);
    }
    if (state.blink && lo == hi && !(state.search.open && state.search.focus)) {
        int cx, cy, height; writer_position(snapshot()->caret, &cx, &cy, &height);
        rect(content, content.x + cx, content.y + cy - state.scroll + 2, 1, height - 4, COLOR_BLACK);
    }
    int track_x = x + w - 12, track_y = page.y;
    rect(all, track_x, track_y, 5, page.h, COLOR_LTGRAY);
    if (state.content_height > page.h && page.h > 0) {
        int thumb = page.h * page.h / state.content_height; if (thumb < 16) thumb = 16;
        if (thumb > page.h) thumb = page.h;
        int top = state.scroll * (page.h - thumb) / (state.content_height - page.h);
        rect(all, track_x, track_y + top, 5, thumb, COLOR_GRAY);
    }
    draw_search(all);
    rect(all, x, y + h - FOOT_H, w, FOOT_H, gfx_rgb(237, 241, 247));
    char foot[112]; const char *prefix = writer_dirty() ? "Unsaved | " : "Saved | ";
    text_copy(foot, sizeof(foot), prefix);
    unsigned offset = 0; while (foot[offset]) offset++;
    text_copy(foot + offset, sizeof(foot) - offset, state.status);
    char counts[48] = ""; append_number(counts, sizeof(counts), writer_word_count());
    unsigned used = string_length(counts); text_copy(counts + used, sizeof(counts) - used, " words | ");
    append_number(counts, sizeof(counts), d->length);
    used = string_length(counts); text_copy(counts + used, sizeof(counts) - used, " bytes");
    int stats_w = ui_string_w(counts);
    Clip foot_clip = {x + 8, y + h - FOOT_H, w - stats_w - 28, FOOT_H};
    label(foot_clip, foot, x + 8, y + h - FOOT_H + 4, COLOR_DKGRAY);
    label(all, counts, x + w - stats_w - 8, y + h - FOOT_H + 4, COLOR_DKGRAY);
}
int writer_click(int x, int y, int w, int h, int mx, int my, int modifiers) {
    if (!state.initialized) writer_init();
    if (mx < x || mx >= x + w || my < y || my >= y + h) return 0;
    geometry(w, h);
    int searched = search_click(x, y, w, h, mx, my, modifiers); if (searched) return searched;
    for (unsigned i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
        const Button *b = &buttons[i];
        if (mx >= x + b->x && mx < x + b->x + b->w && my >= y + b->y && my < y + b->y + 25) {
            state.search.focus = 0;
            int result = button_action(b->action); reveal(); return result;
        }
    }
    int top = y + TOOL_H + 8;
    if (my < top || my >= y + h - FOOT_H - 8 - search_height()) return 0;
    state.search.focus = 0;
    if (mx >= x + w - 18) {
        state.manual_scroll = 1;
        int range = state.content_height - state.viewport_height;
        if (range > 0) state.scroll = clamp((my - top) * range / state.viewport_height, 0, range);
        return WRITER_CHANGED;
    }
    state.search.focus = 0;
    move_hit(mx - x - PAGE_PAD, my - top + state.scroll, modifiers & WRITER_MOD_SHIFT, 0);
    state.dragging = 1; return WRITER_CHANGED;
}
int writer_drag(int x, int y, int w, int h, int mx, int my) {
    if (!state.dragging) return 0;
    geometry(w, h);
    int top = y + TOOL_H + 8, bottom = y + h - FOOT_H - 8 - search_height();
    if (my < top) writer_scroll(-1); else if (my >= bottom) writer_scroll(1);
    move_hit(mx - x - PAGE_PAD, my - top + state.scroll, 1, 0);
    return WRITER_CHANGED;
}
