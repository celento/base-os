/* Bounded rich-text Writer. All persistent working storage lives in its arena. */
#include "apps/writer.h"
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

#define HISTORY 9u
#define LINE_MAX (WRITER_TEXT_MAX + 1u)
#define EXPORT_CAPACITY 524288u
#define UI_HEIGHT 18
#define BODY_HEIGHT 24
#define HEADING_HEIGHT 38

typedef struct {
    WriterDoc doc;
    unsigned caret, anchor, typing_style, affinity;
    DocumentRevision revision;
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
_Static_assert(sizeof(WriterArena) == 1999164u, "Writer footprint changed");
_Static_assert(sizeof(WriterArena) <= WRITER_CAPACITY, "Writer arena overflow");
#ifdef WRITER_HOST_TEST
static WriterArena host_arena;
#define ARENA (&host_arena)
#else
#define ARENA ((WriterArena *)WRITER_BASE)
#endif

static struct {
    unsigned first, count, current;
    DocumentRevision next_revision, saved_revision;
    unsigned clipboard_generation, clipboard_length;
    unsigned line_count;
    DocumentRevision layout_revision;
    int initialized, file, failed_save, clipboard_owned, dragging;
    unsigned identity, binding_incarnation;
    WriterBinding binding;
    int has_binding, binding_conflict;
    DocumentSave save;
    struct {
        int valid, parent;
        unsigned parent_identity, kind, option;
        DocumentTarget target;
        DocumentRevision revision;
        char name[FS_NAME_LEN];
    } export_retry;
    unsigned group_kind, group_caret, group_ticks, group_count;
    DocumentRevision stats_revision;
    unsigned words;
    struct {
        int open, replacing, focus, field, exact;
        unsigned caret[2], anchor[2];
        char text[2][64];
    } search;
    int width, content_height, scroll, viewport_height, desired_x, manual_scroll;
    int layout_valid, blink, last_blink, hit_affinity;
    int inactive, grab; /* window lacks focus; scrollbar thumb grab offset */
    char title[64], status[112];
} state;
_Static_assert(sizeof(state.export_retry) <= 48u + FS_NAME_LEN, "Writer export retry exceeds budget");

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
static int default_text_width(void);
static int default_viewport(void);
static void invalidate(void) { state.layout_valid = 0; state.desired_x = -1; state.blink = 1; }

/* Each complete operation records one state. Older states drop as a ring. */
static int revision_available(void) {
    DocumentRevision last = state.next_revision, next;
    if (document_revision_next(&last, &next)) return 1;
    status("Revision capacity exhausted. Work is unchanged."); return 0;
}
static int next_revision(DocumentRevision *out) {
    if (document_revision_next(&state.next_revision, out)) return 1;
    status("Revision capacity exhausted. Work is unchanged."); return 0;
}
static Snapshot *begin_edit(void) {
    DocumentRevision revision;
    if (!next_revision(&revision)) return 0;
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
    s->revision = revision;
    invalidate();
    return s;
}
static int accept_staging(unsigned caret, unsigned typing_style, unsigned group) {
    unsigned now = timer_ticks();
    int join = group && state.group_kind == group && state.group_caret == snapshot()->caret &&
               snapshot()->caret == snapshot()->anchor && state.current + 1 == state.count &&
               !document_revision_equal(snapshot()->revision, state.saved_revision) && state.group_count < 512u &&
               now - state.group_ticks < TIMER_HZ;
    unsigned count = join ? state.group_count + 1 : 1;
    Snapshot *s;
    if (join) {
        DocumentRevision revision;
        if (!next_revision(&revision)) return 0;
        s = snapshot(); s->revision = revision; invalidate();
    } else { s = begin_edit(); if (!s) return 0; }
    copy_bytes(&s->doc, &ARENA->staging, sizeof(WriterDoc));
    s->caret = s->anchor = caret; s->affinity = 0;
    s->typing_style = typing_style & WRITER_STYLE_MASK;
    state.group_kind = group; state.group_caret = caret; state.group_ticks = now; state.group_count = count;
    status("Edited. Ctrl+S saves the native document."); return 1;
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
    return accept_staging(lo + length, typing, group);
}
static int format_inline(unsigned mask) {
    unsigned lo = selection_low(), hi = selection_high(), typing = snapshot()->typing_style;
    int turn_on = 0;
    if (lo == hi) turn_on = !(typing & mask);
    else for (unsigned i = lo; i < hi; i++) if (!(document()->style[i] & mask)) { turn_on = 1; break; }
    Snapshot *s = begin_edit();
    if (!s) return WRITER_CHANGED;
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
    if (!s) return WRITER_CHANGED;
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
    if (!revision_available()) return;
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
    if (state.layout_valid && state.width == width && document_revision_equal(state.layout_revision, snapshot()->revision)) return;
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
    writer_layout(state.width ? state.width : default_text_width());
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
    writer_layout(state.width ? state.width : default_text_width());
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
    if (!document_revision_equal(state.stats_revision, snapshot()->revision)) {
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
    if (!revision_available()) return;
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
    if (!accept_staging(0, snapshot()->typing_style, 0)) return;
    reveal();
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
    DocumentRevision revision;
    if (!next_revision(&revision)) return;
    document_save_detach(&state.save);
    state.export_retry.valid = 0;
    state.search.open = state.search.focus = 0;
    state.group_kind = 0;
    state.first = state.current = 0; state.count = 1;
    Snapshot *s = snapshot(); writer_doc_init(&s->doc);
    s->caret = s->anchor = s->typing_style = s->affinity = 0;
    s->revision = revision; state.saved_revision = s->revision;
    state.file = -1; state.identity = state.binding_incarnation = 0; state.has_binding = state.binding_conflict = 0; state.failed_save = state.dragging = 0;
    state.scroll = state.manual_scroll = 0; text_copy(state.title, sizeof(state.title), "Untitled");
    invalidate(); status("New document. Body text, printable ASCII and tabs.");
}
void writer_init(void) {
    if (state.initialized) return;
    state.initialized = 1; state.width = default_text_width();
    state.viewport_height = default_viewport();
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
    DocumentRevision revision;
    if (!next_revision(&revision)) return 0;
    document_save_detach(&state.save);
    state.export_retry.valid = 0;
    state.group_kind = 0;
    state.search.open = state.search.focus = 0;
    state.first = state.current = 0; state.count = 1;
    Snapshot *s = snapshot(); copy_bytes(&s->doc, &ARENA->staging, sizeof(WriterDoc));
    s->caret = s->anchor = s->affinity = 0; s->typing_style = s->doc.style[0];
    s->revision = revision; state.saved_revision = native ? s->revision : DOCUMENT_REVISION_NONE;
    state.file = native ? id : -1; state.identity = native ? fs_identity(id) : 0;
    state.binding_incarnation = native ? fs_incarnation() : 0;
    state.has_binding = native; state.binding_conflict = 0;
    if (native) fingerprint(data, (unsigned)size, &state.binding);
    state.failed_save = state.dragging = 0; state.scroll = state.manual_scroll = 0;
    text_copy(state.title, sizeof(state.title), fs_name(id));
    invalidate(); status(native ? "Opened native document." : "Imported text. Save As a new .bwr to retain formatting."); return 1;
}
static int binding_valid(void) {
    return state.file >= 0 && fs_valid(state.file) && !fs_is_dir(state.file) && !fs_is_app(state.file) &&
           native_name(fs_name(state.file)) && state.identity && state.binding_incarnation &&
           fs_incarnation() == state.binding_incarnation && fs_identity(state.file) == state.identity &&
           fs_content_revision(state.file) != 0;
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
static void persistence_complete(void) {
    if (state.save.kind == DOCUMENT_SAVE_NATIVE) {
        if (state.save.result == BOS_OK && (!binding_valid() || state.file != state.save.target.file ||
            state.identity != state.save.target.identity || !document_target_matches(&state.save.target)))
            state.save.result = BOS_E_STALE;
        if (state.save.result == BOS_OK) {
            state.saved_revision = state.save.revision; state.failed_save = 0;
            text_copy(state.title, sizeof(state.title), fs_name(state.file));
            status(document_revision_equal(snapshot()->revision, state.saved_revision) ?
                   "Saved to disk." : "Saved earlier version. New edits remain unsaved.");
        } else { state.failed_save = 1; status(document_save_error(&state.save)); }
    } else if (state.save.result == BOS_OK) {
        state.export_retry.valid = 0;
        status(state.save.kind == DOCUMENT_SAVE_PDF ? "PDF saved to disk. Native state unchanged." :
               "RTF saved to disk. Native state unchanged.");
    } else status(document_save_error(&state.save));
}
int writer_persistence_poll(void) {
    if (!state.initialized || !document_save_poll(&state.save)) return 0;
    persistence_complete(); return WRITER_CHANGED;
}
void writer_save_info(DocumentSave *out) { if (out) *out = state.save; }
int writer_export_retry_name(unsigned kind, char *out, unsigned capacity, unsigned *option) {
    if (!out || !capacity || !state.export_retry.valid || state.export_retry.kind != kind ||
        !document_revision_equal(snapshot()->revision, state.export_retry.revision) ||
        !document_target_matches(&state.export_retry.target) ||
        fs_identity(state.export_retry.parent) != state.export_retry.parent_identity ||
        fs_find_child(state.export_retry.parent, state.export_retry.name) != state.export_retry.target.file ||
        string_length(state.export_retry.name) >= capacity) return 0;
    text_copy(out, capacity, state.export_retry.name);
    if (option) *option = state.export_retry.option;
    return 1;
}
static int persistence_ready(void) {
    if (!document_save_ensure_owner(&state.save)) {
        status("Save owner capacity exhausted. Work is unchanged."); return 0;
    }
    if (!fs_version_available(0)) {
        status("File version capacity exhausted. Work is unchanged."); return 0;
    }
    return 1;
}
static void rollback_empty(int file, unsigned incarnation, unsigned identity) {
    if (identity && incarnation == fs_incarnation() && fs_valid(file) && fs_identity(file) == identity &&
        !fs_is_dir(file) && !fs_is_app(file) && !fs_size(file)) (void)fs_delete(file);
}
static int save_to(int id, unsigned length, int *installed) {
    *installed = 0;
    if (!fs_version_available(0)) { status("File version capacity exhausted. Work is unchanged."); return WRITER_SAVE_ERROR; }
    int written = fs_write(id, (const char *)ARENA->output, (int)length);
    if (written == FS_ERR_BUSY) { status("Disk is saving; retry shortly."); return WRITER_SAVE_ERROR; }
    if (written != (int)length) {
        state.failed_save = 1; status("Save failed; the complete document remains open."); return WRITER_SAVE_ERROR;
    }
    *installed = 1;
    /* RAM binding and durable baseline are independent. This complete target
     * is ours even if admission or the eventual durability boundary fails. */
    state.file = id; state.identity = fs_identity(id); state.binding_incarnation = fs_incarnation();
    state.has_binding = 1; state.binding_conflict = 0; state.failed_save = 1;
    fingerprint(ARENA->output, length, &state.binding);
    text_copy(state.title, sizeof(state.title), fs_name(id));
    int result = document_save_begin(&state.save, DOCUMENT_SAVE_NATIVE, snapshot()->revision, id, length, 0);
    if (result == DOCUMENT_SAVE_PENDING) status("Saving to disk. You can keep editing.");
    else persistence_complete();
    return result;
}
int writer_save(void) {
    if (state.save.pending) {
        status(state.save.kind == DOCUMENT_SAVE_NATIVE ? "Save in progress. New edits remain private." : "Export in progress; retry Save shortly.");
        return state.save.kind == DOCUMENT_SAVE_NATIVE ? WRITER_SAVE_PENDING : WRITER_SAVE_ERROR;
    }
    if (fs_sync_busy()) { status("Disk is saving; retry shortly."); return WRITER_SAVE_ERROR; }
    if (!state.initialized) writer_init();
    if (!binding_valid()) { status("Choose Save As for a new native .bwr file."); return WRITER_SAVE_NEEDS_NAME; }
    if (!state.has_binding || !writer_binding_matches(state.file, &state.binding)) {
        state.binding_conflict = 1;
        status("The source file changed outside Writer. Save As a new .bwr; the source is unchanged.");
        return WRITER_SAVE_NEEDS_NAME;
    }
    if (!persistence_ready()) return WRITER_SAVE_ERROR;
    state.group_kind = 0;
    unsigned length;
    if (!writer_snapshot(&length)) return WRITER_SAVE_ERROR;
    int installed; return save_to(state.file, length, &installed);
}
int writer_save_as(int parent, const char *name) {
    if (state.save.pending || fs_sync_busy()) { status("Disk is saving; retry shortly."); return WRITER_SAVE_ERROR; }
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
    if (!persistence_ready() || !fs_version_available(1)) {
        status("Save identity capacity unavailable. Work is unchanged."); return WRITER_SAVE_ERROR;
    }
    state.group_kind = 0;
    unsigned incarnation = fs_incarnation();
    int id = fs_create(parent, name);
    if (id < 0) { status("Could not create that file; check name, space and disk status."); return WRITER_SAVE_ERROR; }
    unsigned identity = fs_identity(id);
    int installed, result = save_to(id, length, &installed);
    if (!installed) rollback_empty(id, incarnation, identity);
    return result;
}
static int export_name(int parent, const char *name, unsigned kind, unsigned option) {
    unsigned n = 0;
    unsigned longest = (unsigned)fs_name_max();
    if (name) while (n <= longest && name[n] && name[n] != '/') ++n;
    if (!name || !n || n > longest || name[n]) {
        status(longest > 23 ? "Choose an export name of 1-255 characters, with no slash."
                            : "Choose an export name of 1-23 characters, with no slash."); return 0;
    }
    if (!extension(name, kind == DOCUMENT_SAVE_PDF ? "pdf" : "rtf")) {
        status(kind == DOCUMENT_SAVE_PDF ? "PDF exports use the .pdf filename extension." : "RTF exports use the .rtf filename extension."); return 0;
    }
    if (!fs_is_dir(parent) || !fs_identity(parent)) { status("The export folder is unavailable; choose a valid folder."); return 0; }
    if (kind == DOCUMENT_SAVE_PDF && option != WRITER_PDF_LETTER && option != WRITER_PDF_A4) {
        status("Choose Letter or A4 paper; no PDF was created."); return 0;
    }
    return 1;
}
static int export_to(int parent, const char *name, unsigned kind, unsigned option) {
    if (state.save.pending || fs_sync_busy()) { status("Disk is saving; retry shortly."); return -1; }
    if (!state.initialized) writer_init();
    if (!export_name(parent, name, kind, option)) return -1;
    int existing = fs_find_child(parent, name);
    if (existing >= 0) {
        if (!state.export_retry.valid || state.export_retry.kind != kind || state.export_retry.parent != parent ||
            state.export_retry.parent_identity != fs_identity(parent) || kstrcmp(state.export_retry.name, name) ||
            existing != state.export_retry.target.file || !document_target_matches(&state.export_retry.target)) {
            status("Choose a new export name; existing files are never replaced."); return -1;
        }
        if (!document_revision_equal(snapshot()->revision, state.export_retry.revision) || option != state.export_retry.option) {
            status("Pending export differs from this document or paper. Choose a new name."); return -1;
        }
    }
    unsigned length, pages = 0;
    if (kind == DOCUMENT_SAVE_PDF) {
        if (writer_pdf_export(document(), option, 0, 0, &length, &pages) != WRITER_PDF_OK) {
            status("Could not measure PDF; the native document is unchanged."); return -1;
        }
        if (length > EXPORT_CAPACITY) {
            status("PDF exceeds the 512 KiB export buffer. Simplify styles or split the document."); return -1;
        }
    } else if (writer_rtf_export(document(), ARENA->output, EXPORT_CAPACITY, &length)) {
        status("RTF export exceeds its 512 KiB bound; simplify styles or split the document."); return -1;
    }
    if (length > fs_file_limit()) { status("Export exceeds this volume's per-file limit; no file was created."); return -1; }
    if (existing < 0) {
        unsigned count = (unsigned)fs_node_count(), capacity = fs_capacity_for_nodes(count + 1), used = fs_used_bytes();
        if (count >= (unsigned)fs_node_limit()) { status("The volume has no free file slots; no export was created."); return -1; }
        if (used > capacity || length > capacity - used) { status("Not enough space for the complete export; no file was created."); return -1; }
    }
    if (kind == DOCUMENT_SAVE_PDF) {
        unsigned written, actual_pages;
        if (writer_pdf_export(document(), option, ARENA->output, EXPORT_CAPACITY, &written, &actual_pages) != WRITER_PDF_OK ||
            written != length || actual_pages != pages) { status("Could not serialize the complete PDF; no export was created."); return -1; }
    }
    int id = existing;
    if (id >= 0) {
        const unsigned char *bytes = (const unsigned char *)fs_data(id);
        if (!bytes || fs_size(id) != (int)length || length != state.export_retry.target.length) {
            status("The pending export changed outside Writer. Choose a new name."); return -1;
        }
        for (unsigned i = 0; i < length; ++i) {
            if (bytes[i] != ARENA->output[i]) { status("The pending export changed outside Writer. Choose a new name."); return -1; }
            if ((i & 4095u) == 4095u) platform_poll();
        }
        if (!document_save_ensure_owner(&state.save)) { status("Save owner capacity exhausted. Work is unchanged."); return -1; }
    } else {
        if (!persistence_ready() || !fs_version_available(1)) { status("Save identity capacity unavailable. Work is unchanged."); return -1; }
        unsigned incarnation = fs_incarnation();
        id = fs_create(parent, name);
        if (id < 0) { status("Could not create the export; check filename, folder, space and disk status."); return -1; }
        unsigned identity = fs_identity(id);
        if (!fs_version_available(0) || fs_write(id, (const char *)ARENA->output, (int)length) != (int)length) {
            rollback_empty(id, incarnation, identity);
            status("Export write failed. Native work is unchanged; no complete export was written."); return -1;
        }
        DocumentTarget target;
        if (!document_target_capture(id, length, &target)) { status("Export remains in RAM; file version is unavailable."); return -1; }
        /* Replace only after a complete new output is installed. Never delete
         * an older complete export when replacing its retry authority. */
        state.export_retry.valid = 1; state.export_retry.parent = parent;
        state.export_retry.parent_identity = fs_identity(parent); state.export_retry.kind = kind;
        state.export_retry.option = option; state.export_retry.target = target;
        state.export_retry.revision = snapshot()->revision;
        text_copy(state.export_retry.name, sizeof(state.export_retry.name), name);
    }
    state.group_kind = 0;
    int result = document_save_begin(&state.save, kind, snapshot()->revision, id, length, option);
    if (result == DOCUMENT_SAVE_PENDING) { status("Exporting to disk. Native state unchanged."); return WRITER_EXPORT_PENDING; }
    persistence_complete(); return result == DOCUMENT_SAVE_OK ? id : -1;
}
int writer_export_rtf(int parent, const char *name) { return export_to(parent, name, DOCUMENT_SAVE_RTF, 0); }
int writer_export_pdf(int parent, const char *name, unsigned paper) { return export_to(parent, name, DOCUMENT_SAVE_PDF, paper); }
int writer_restore(const unsigned char *data, unsigned length, int file, unsigned identity,
                   int dirty, unsigned caret, unsigned anchor) {
    if (!state.initialized) writer_init();
    if (writer_native_decode(&ARENA->staging, data, length)) { status("Invalid recovery document; current work is unchanged."); return 0; }
    DocumentRevision revision;
    if (!next_revision(&revision)) return 0;
    document_save_detach(&state.save);
    state.export_retry.valid = 0;
    state.group_kind = 0;
    state.search.open = state.search.focus = 0;
    state.first = state.current = 0; state.count = 1;
    Snapshot *s = snapshot(); copy_bytes(&s->doc, &ARENA->staging, sizeof(WriterDoc));
    s->affinity = 0; s->caret = min_u(caret, s->doc.length); s->anchor = min_u(anchor, s->doc.length);
    s->typing_style = s->doc.style[s->caret]; s->revision = revision;
    state.file = file; state.identity = identity; state.binding_incarnation = fs_incarnation();
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
    state.saved_revision = dirty ? DOCUMENT_REVISION_NONE : s->revision; state.failed_save = 0;
    text_copy(state.title, sizeof(state.title), state.file >= 0 ? fs_name(state.file) : "Recovered document");
    state.scroll = state.dragging = state.manual_scroll = 0; invalidate(); status("Recovered document draft."); return 1;
}
const char *writer_title(void) { return state.initialized ? state.title : "Untitled"; }
const char *writer_status(void) { return state.status; }
int writer_dirty(void) { return state.initialized && (state.failed_save || state.binding_conflict || !document_revision_equal(snapshot()->revision, state.saved_revision) || (state.file >= 0 && !binding_valid())); }
int writer_read_only(void) { return 0; } /* All valid imports are editable copies. */
int writer_file(void) { return state.initialized && binding_valid() ? state.file : -1; }
unsigned writer_file_identity(void) { return state.initialized && binding_valid() ? state.identity : 0; }
unsigned writer_length(void) { return state.initialized ? document()->length : 0; }
unsigned writer_caret(void) { return state.initialized ? snapshot()->caret : 0; }
unsigned writer_anchor(void) { return state.initialized ? snapshot()->anchor : 0; }
const WriterDoc *writer_document(void) { if (!state.initialized) writer_init(); return document(); }
unsigned writer_line_count(void) { writer_layout(state.width); return state.line_count; }
void writer_close(void) { if (state.initialized) writer_new(); document_save_detach(&state.save); state.export_retry.valid = 0; }
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
        case 0x1f: return extend ? WRITER_REQUEST_SAVE_AS : WRITER_REQUEST_SAVE;
        case 0x12: if (extend) return WRITER_REQUEST_EXPORT; return format_paragraph(WRITER_ALIGN_MASK, WRITER_ALIGN_CENTER);
        case 0x19: if (extend) return WRITER_REQUEST_PDF; return 0;
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
        case 0x2d: if (selection_low() != selection_high() && !revision_available()) return WRITER_CHANGED;
            copy_selection(); if (selection_low() != selection_high()) replace(selection_low(), selection_high(), 0, 0, 0, 0, 0); reveal(); return WRITER_CHANGED;
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
static Clip intersect(Clip a, Clip b) {
    int right = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w;
    int bottom = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
    a.x = a.x > b.x ? a.x : b.x; a.y = a.y > b.y ? a.y : b.y;
    a.w = right > a.x ? right - a.x : 0; a.h = bottom > a.y ? bottom - a.y : 0; return a;
}
/* Kit controls paint unclipped, so a control is shown and hit only when it
 * lies wholly inside the client area (window-relative box). */
static int box_visible(Clip b, int w, int h) { return b.x >= 0 && b.y >= 0 && b.x + b.w <= w && b.y + b.h <= h; }

/* The desktop overrides this weak default to report pointer hover and press
 * for a control rectangle (absolute coordinates) and to repaint on change. */
__attribute__((weak)) int writer_hover(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; return 0; }
void writer_set_active(int active) { state.inactive = !active; }
static int hot(int x, int y, int w, int h) { return state.inactive ? 0 : writer_hover(x, y, w, h); }

/* Style metrics. Classic: square controls on TK_BASE strips with subtle
 * rules; Modern: rounded controls, and the toolbar continues the header. */
static int modern(void) { return ui_theme.style == UI_STYLE_MODERN; }
static int ctl(void) { return ui_theme.control_h; }
static int tool_row_h(void) { return modern() ? 44 : 34; }
static int tool_row_gap(void) { return modern() ? 6 : 4; }
static int tool_margin(void) { return modern() ? 12 : 8; }
/* The ruler gives way to the text in short windows. */
static int ruler_h(int h) { return h < 320 ? 0 : modern() ? 24 : 22; }
static int status_h(void) { return modern() ? 28 : 24; }
static int desk_gap(void) { return modern() ? 16 : 12; }
#define PAGE_MAX 816      /* Letter width at 96 px per inch */
#define PAGE_PAD_V 24
#define RULER_STEP 12     /* an eighth of a 96 px inch */

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

/* Main toolbar. Groups flow left to right with dividers; Find sits at the
 * right edge. Wide windows use a single row; narrower ones wrap the
 * history/file groups onto a second row. Drawing and hit testing consume
 * this same geometry. */
enum { ACT_BOLD = 1, ACT_ITALIC, ACT_UNDERLINE, ACT_BODY, ACT_HEADING, ACT_LEFT, ACT_CENTER, ACT_RIGHT,
       ACT_UNDO, ACT_REDO, ACT_SAVE, ACT_RTF, ACT_FIND, ACT_PDF };
typedef struct { unsigned char action, group; signed char icon; const char *label; } ToolItem;
static const ToolItem tool_items[] = {
    {ACT_BOLD, 0, ICON_BOLD, 0}, {ACT_ITALIC, 0, ICON_ITALIC, 0}, {ACT_UNDERLINE, 0, ICON_UNDERLINE, 0},
    {ACT_BODY, 1, ICON_BODY, 0}, {ACT_HEADING, 1, ICON_HEADING, 0},
    {ACT_LEFT, 2, ICON_ALIGN_LEFT, 0}, {ACT_CENTER, 2, ICON_ALIGN_CENTER, 0}, {ACT_RIGHT, 2, ICON_ALIGN_RIGHT, 0},
    {ACT_UNDO, 3, ICON_UNDO, 0}, {ACT_REDO, 3, ICON_REDO, 0},
    {ACT_SAVE, 4, ICON_SAVE, 0}, {ACT_RTF, 4, ICON_FILE_TYPE, "RTF"}, {ACT_PDF, 4, ICON_EXPORT, "PDF"},
    {ACT_FIND, 5, ICON_SEARCH, 0}
};
#define TOOL_COUNT (sizeof tool_items / sizeof tool_items[0])
#define TOOL_WRAP_GROUP 3   /* first group of the second row when wrapped */
#define TOOL_RIGHT_GROUP 5  /* right-aligned group */
typedef struct { int height, dividers; Clip item[TOOL_COUNT]; Clip divider[TOOL_RIGHT_GROUP]; } Toolbar;
static int tool_w(const char *text) { return text ? ICON_SIZE + 6 + kit_text_w(text, 0) + 22 : ctl(); }
/* Returns the right edge of the left-flowing groups. */
static int toolbar_flow(int w, int wrap, Toolbar *t) {
    int s = ctl(), y0 = (tool_row_h() - 1 - s) / 2, ys[2] = {y0, y0 + s + tool_row_gap()};
    int row = 0, x = tool_margin(), right = 0, group = -1;
    t->height = tool_row_h() + (wrap ? s + tool_row_gap() : 0); t->dividers = 0;
    for (unsigned i = 0; i < TOOL_COUNT; i++) {
        const ToolItem *it = &tool_items[i]; int bw = tool_w(it->label);
        if (it->group == TOOL_RIGHT_GROUP) { t->item[i] = (Clip){w - tool_margin() - bw, ys[0], bw, s}; continue; }
        if (it->group != group && group >= 0) {
            if (wrap && it->group == TOOL_WRAP_GROUP) { row = 1; x = tool_margin(); }
            else { t->divider[t->dividers++] = (Clip){x + 6, ys[row] + 5, 1, s - 10}; x += 13; }
        } else if (group >= 0) x += 4;
        group = it->group;
        t->item[i] = (Clip){x, ys[row], bw, s};
        x += bw; if (x > right) right = x;
    }
    return right;
}
static void toolbar_layout(int w, Toolbar *t) {
    if (toolbar_flow(w, 0, t) + 13 + ctl() + tool_margin() > w) toolbar_flow(w, 1, t);
}
static int button_active(unsigned action) {
    unsigned style = snapshot()->typing_style, para = paragraph_at(document(), snapshot()->caret);
    if (action <= 3) return !!(style & (1u << (action - 1)));
    if (action == 4 || action == 5) return !!(para & WRITER_PARAGRAPH_HEADING) == (action == 5);
    if (action >= 6 && action <= 8) return (para & WRITER_ALIGN_MASK) == action - 6;
    return 0;
}
/* Toggles show their state; Undo/Redo dim when history has nothing to give. */
static int button_state(unsigned action) {
    if (action <= ACT_RIGHT) return button_active(action) ? KIT_ON : 0;
    if (action == ACT_UNDO && !state.current) return KIT_DISABLED;
    if (action == ACT_REDO && state.current + 1 >= state.count) return KIT_DISABLED;
    if (action == ACT_FIND && state.search.open) return KIT_ON;
    return 0;
}
static int button_action(unsigned action) {
    if (action <= 3) return format_inline(1u << (action - 1));
    if (action == 4 || action == 5) return format_paragraph(WRITER_PARAGRAPH_HEADING, action == 5 ? WRITER_PARAGRAPH_HEADING : 0);
    if (action >= 6 && action <= 8) return format_paragraph(WRITER_ALIGN_MASK, action - 6);
    if (action == 9 || action == 10) return undo(action == 10);
    if (action == 11) return WRITER_REQUEST_SAVE;
    if (action == 12) return WRITER_REQUEST_EXPORT;
    if (action == 14) return WRITER_REQUEST_PDF;
    /* Find toggles the find bar, matching its on state. */
    if (state.search.open) state.search.open = state.search.focus = 0;
    else search_open(0);
    return WRITER_CHANGED;
}

/* Window-relative frame: toolbar, ruler, the desk with its page and
 * scrollbar, the docked Classic find bar and the status bar. */
typedef struct {
    Toolbar bar;
    int ruler_y, ruler_h, desk_y, desk_h, find_y, find_h, status_y;
    int sb_x, page_x, page_w, text_x, text_w, text_y, viewport;
} Frame;
static int find_rows(void) { return state.search.replacing ? 2 : 1; }
static void frame(int w, int h, Frame *f) {
    toolbar_layout(w, &f->bar);
    f->ruler_y = f->bar.height;
    f->ruler_h = ruler_h(h);
    f->desk_y = f->ruler_y + f->ruler_h;
    f->status_y = h - status_h();
    f->find_h = state.search.open && !modern() ? find_rows() * (ctl() + 8) + 4 : 0;
    f->find_y = f->status_y - f->find_h;
    f->desk_h = f->find_y - f->desk_y; if (f->desk_h < 0) f->desk_h = 0;
    f->sb_x = w - ui_theme.scrollbar;
    int gap = desk_gap();
    f->page_w = clamp(f->sb_x - 2 * gap, 64, PAGE_MAX);
    f->page_x = (f->sb_x - f->page_w) / 2;
    int pad = f->page_w >= 560 ? 48 : 24;
    f->text_x = f->page_x + pad; f->text_w = f->page_w - 2 * pad;
    if (f->text_w < 32) f->text_w = 32;
    f->text_y = f->desk_y + gap + PAGE_PAD_V;
    f->viewport = f->desk_h - 2 * (gap + PAGE_PAD_V);
    if (f->viewport < 24) f->viewport = 24;
}
static int default_text_width(void) { Frame f; frame(WRITER_W, WRITER_H, &f); return f.text_w; }
static int default_viewport(void) { Frame f; frame(WRITER_W, WRITER_H, &f); return f.viewport; }
static void geometry(int w, int h, Frame *f) {
    int old_width = state.width, old_height = state.viewport_height;
    frame(w, h, f);
    state.viewport_height = f->viewport;
    writer_layout(f->text_w);
    if (!state.manual_scroll && (old_width != state.width || old_height != state.viewport_height)) reveal();
}

/* Find bar: Classic docks it above the status bar, Modern floats it at the
 * top right of the page. One geometry routine feeds drawing and hits. */
enum { FIND_FIELD, FIND_WITH, FIND_PREV, FIND_NEXT, FIND_CASE, FIND_CLOSE, FIND_REPLACE, FIND_ALL, FIND_COUNT };
typedef struct { Clip panel, item[FIND_COUNT]; int count_x, right, shown; } FindBar;
static void find_layout(const Frame *f, int w, int h, FindBar *L) {
    int s = ctl(), rows = find_rows();
    if (modern()) {
        int pw = w - 48 - ui_theme.scrollbar; if (pw > 500) pw = 500;
        L->panel = (Clip){f->sb_x - 12 - pw, f->desk_y + 10, pw, rows * (s + 8) + 8};
        /* The panel and its two-band shadow must fit inside the client. */
        L->shown = pw > 0 && box_visible((Clip){L->panel.x, L->panel.y, L->panel.w, L->panel.h + 4}, w, h);
    } else {
        L->panel = (Clip){0, f->find_y, w, f->find_h};
        L->shown = 1;
    }
    int x0 = L->panel.x + 8, y0 = L->panel.y + (modern() ? 8 : 6), y1 = y0 + s + 8;
    int count_w = kit_text_w("999 of 999", 0), case_w = modern() ? s + 4 : kit_text_w("Match case", 0) + 24;
    int need = 10 + count_w + 2 * s + 2 + 12 + case_w + (modern() ? 12 : 8) + s, fw = modern() ? 210 : 240;
    L->right = L->panel.x + L->panel.w - 8;
    if (x0 + fw + need > L->right) fw = L->right - x0 - need;
    if (fw < 60) fw = 60;
    L->count_x = x0 + fw + 10;
    L->item[FIND_FIELD] = (Clip){x0, y0, fw, s};
    L->item[FIND_WITH] = (Clip){x0, y1, fw, s};
    L->item[FIND_PREV] = (Clip){L->count_x + count_w, y0, s, s};
    L->item[FIND_NEXT] = (Clip){L->count_x + count_w + s + 2, y0, s, s};
    L->item[FIND_CASE] = (Clip){L->item[FIND_NEXT].x + s + 12, y0, case_w, s};
    L->item[FIND_CLOSE] = (Clip){L->right - s, y0, s, s};
    L->item[FIND_REPLACE] = (Clip){L->count_x, y1, kit_button_w("Replace", -1), s};
    L->item[FIND_ALL] = (Clip){L->count_x + L->item[FIND_REPLACE].w + 8, y1, kit_button_w("Replace All", -1), s};
}
/* An item shows (and hits) when its row is open, it does not run into Close
 * or the panel edge, and it fits in the client with room for a focus ring. */
static int find_item(const FindBar *L, unsigned i, int w, int h) {
    if (!L->shown || ((i == FIND_WITH || i == FIND_REPLACE || i == FIND_ALL) && !state.search.replacing)) return 0;
    Clip b = L->item[i];
    int limit = i != FIND_CLOSE && b.y == L->item[FIND_CLOSE].y ? L->item[FIND_CLOSE].x - 4 : L->right;
    if (b.x + b.w > limit) return 0;
    return box_visible((Clip){b.x - 2, b.y - 2, b.w + 4, b.h + 4}, w, h);
}
static int field_text_x(unsigned field) { return (modern() ? 11 : 7) + (field ? 0 : 21); }
static int field_text_w(Clip r, unsigned field) { int n = r.w - field_text_x(field) - 6; return n > 0 ? n : 0; }
static int search_offset(unsigned field, int available) {
    int prefix = 0;
    for (unsigned i = 0; i < state.search.caret[field]; i++) prefix += ui_advance(state.search.text[field][i]);
    return prefix >= available ? prefix - available + 8 : 0;
}
/* Non-overlapping matches, counted as Replace All counts them, and which
 * one the selection is. */
static void search_count(unsigned *count, unsigned *current) {
    const WriterDoc *d = document(); unsigned n = string_length(state.search.text[0]);
    unsigned lo = selection_low(), hi = selection_high();
    *count = *current = 0;
    if (!n) return;
    for (unsigned i = 0; i + n <= d->length;) {
        if (search_match(i)) {
            ++*count;
            if (i == lo && hi - lo == n) *current = *count;
            i += n;
        } else i++;
        if (!(i & 2047u)) platform_poll();
    }
}
static void draw_find_field(Clip all, Clip r, unsigned field) {
    int ax = all.x + r.x, ay = all.y + r.y;
    int focus = state.search.focus && (unsigned)state.search.field == field && !state.inactive;
    const char *text = state.search.text[field];
    if (field) {
        kit_field(ax, ay, r.w, r.h, focus ? KIT_FOCUS : 0);
        if (!text[0] && !focus) kit_text("Replace with", ax + field_text_x(1), kit_ty(ay, r.h), TK_TEXT2, field_text_w(r, 1), 0);
    } else kit_search(ax, ay, r.w, r.h, "", text[0] ? "" : "Find", focus ? KIT_FOCUS : 0);
    Clip box = intersect(all, (Clip){ax + field_text_x(field), kit_ty(ay, r.h), field_text_w(r, field), UI_FONT_H});
    if (!box.w || !box.h) return;
    int x = ax + field_text_x(field) - search_offset(field, field_text_w(r, field)), caret_x = x;
    unsigned a = state.search.anchor[field], b = state.search.caret[field];
    unsigned lo = a < b ? a : b, hi = a > b ? a : b;
    for (unsigned i = 0; text[i]; i++) {
        int selected = focus && i >= lo && i < hi, width = ui_advance(text[i]);
        if (selected) rect(box, x, box.y, width, box.h, TK_ACCENT);
        glyph(box, (unsigned char)text[i], x, box.y, 0, 0, selected ? TK_ON_ACCENT : TK_TEXT);
        x += width;
        if (i + 1 == b) caret_x = x;
    }
    if (focus && lo == hi && state.blink) rect(box, caret_x, box.y + 1, 1, 15, TK_TEXT);
}
static void draw_find(Clip all, const Frame *f) {
    if (!state.search.open) return;
    FindBar L; find_layout(f, all.w, all.h, &L);
    if (!L.shown) return;
    Clip p = L.panel; int px = all.x + p.x, py = all.y + p.y, s = ctl();
    if (modern()) { kit_shadow(px, py, p.w, p.h, 10); kit_box(px, py, p.w, p.h, 10, ui_theme.overlay, TK_LINE_STRONG); }
    else { rect(all, px, py, p.w, 1, TK_LINE_SUBTLE); rect(all, px, py + 1, p.w, p.h - 1, TK_BASE); }
    for (unsigned i = 0; i < FIND_COUNT; i++) {
        if (!find_item(&L, i, all.w, all.h)) continue;
        Clip b = L.item[i]; int bx = all.x + b.x, by = all.y + b.y, st = hot(bx, by, b.w, b.h);
        if (i == FIND_FIELD || i == FIND_WITH) draw_find_field(all, b, i == FIND_WITH);
        else if (i == FIND_PREV || i == FIND_NEXT) kit_icon_button(bx, by, s, i == FIND_PREV ? UI_ICON_UP : UI_ICON_DOWN, st);
        else if (i == FIND_CLOSE) kit_icon_button(bx, by, s, UI_ICON_X, st);
        else if (i == FIND_REPLACE || i == FIND_ALL) kit_button(bx, by, b.w, b.h, i == FIND_ALL ? "Replace All" : "Replace", -1, KB_DEFAULT, st);
        else if (modern()) {
            if (state.search.exact || st) kit_fill(bx, by, b.w, b.h, 6, state.search.exact ? TK_ACCENT_100 : TK_HOVER, 1);
            kit_text_center("Aa", bx, kit_ty(by, b.h), b.w, state.search.exact ? TK_ACCENT_700 : TK_TEXT, 0);
        } else kit_check_row(bx, by + (b.h - ui_theme.row_h) / 2, "Match case", (state.search.exact ? KIT_ON : 0) | st, 0);
    }
    if (find_item(&L, FIND_PREV, all.w, all.h)) {
        unsigned count, current; search_count(&count, &current);
        char text[32] = "";
        if (state.search.text[0][0] && !count) text_copy(text, sizeof text, "No results");
        else if (count && !current) {
            append_number(text, sizeof text, count);
            unsigned n = string_length(text); text_copy(text + n, sizeof text - n, count == 1 ? " match" : " matches");
        } else if (count) {
            append_number(text, sizeof text, current);
            unsigned n = string_length(text); text_copy(text + n, sizeof text - n, " of ");
            append_number(text, sizeof text, count);
        }
        int cx = all.x + L.count_x;
        kit_text(text, cx, kit_ty(all.y + L.item[FIND_PREV].y, s), TK_TEXT2, all.x + L.item[FIND_PREV].x - 4 - cx, 0);
    }
    if (find_item(&L, FIND_CASE, all.w, all.h))
        kit_vseparator(all.x + L.item[FIND_CASE].x - 6, all.y + L.item[FIND_CASE].y + 4, s - 8);
    if (modern() && find_item(&L, FIND_CLOSE, all.w, all.h))
        kit_vseparator(all.x + L.item[FIND_CLOSE].x - 6, all.y + L.item[FIND_CLOSE].y + 4, s - 8);
}
static int find_click(const Frame *f, int w, int h, int lx, int ly, int modifiers) {
    if (!state.search.open) return 0;
    FindBar L; find_layout(f, w, h, &L);
    Clip p = L.panel;
    if (!L.shown || lx < p.x || lx >= p.x + p.w || ly < p.y || ly >= p.y + p.h) return 0;
    state.group_kind = 0; state.dragging = 0;
    for (unsigned i = 0; i < FIND_COUNT; i++) {
        Clip b = L.item[i];
        if (!find_item(&L, i, w, h) || lx < b.x || lx >= b.x + b.w || ly < b.y || ly >= b.y + b.h) continue;
        if (i == FIND_FIELD || i == FIND_WITH) {
            unsigned field = i == FIND_WITH;
            int local = lx - b.x - field_text_x(field) + search_offset(field, field_text_w(b, field)), used = 0;
            unsigned pos = 0; const char *text = state.search.text[field];
            while (text[pos]) {
                int width = ui_advance(text[pos]); if (local < used + (width + 1) / 2) break;
                used += width; pos++;
            }
            state.search.focus = 1; state.search.field = (int)field; state.search.caret[field] = pos;
            if (!(modifiers & WRITER_MOD_SHIFT)) state.search.anchor[field] = pos;
        } else if (i == FIND_PREV || i == FIND_NEXT) search_find(i == FIND_NEXT ? 1 : -1);
        else if (i == FIND_CASE) { state.search.exact = !state.search.exact; status(state.search.exact ? "Search is case-sensitive." : "Search ignores ASCII letter case."); }
        else if (i == FIND_REPLACE) search_replace_one();
        else if (i == FIND_ALL) search_replace_all();
        else state.search.open = state.search.focus = 0;
        return WRITER_CHANGED;
    }
    return WRITER_CHANGED;
}

/* Ruler over the page: the text column in white with eighth-inch ticks and
 * inch numbers, and margin markers at both edges of the column. */
static void draw_ruler(Clip all, const Frame *f) {
    int y = all.y + f->ruler_y, h = f->ruler_h;
    if (!h) return;
    rect(all, all.x, y, all.w, h - 1, modern() ? ui_theme.content : TK_BASE);
    rect(all, all.x, y + h - 1, all.w, 1, TK_LINE_SUBTLE);
    int left = all.x + f->text_x, right = left + f->text_w, band_y = y + 3, band_h = h - 7;
    rect(all, all.x + f->page_x, band_y, f->page_w, band_h, TK_SUNKEN);
    rect(all, left, band_y, f->text_w, band_h, TK_RAISED);
    int labels = box_visible((Clip){0, f->ruler_y, all.w, h}, all.w, all.h);
    for (int k = 1; left + k * RULER_STEP < right; k++) {
        int px = left + k * RULER_STEP;
        if (k % 8 == 0) {
            char number[12] = ""; append_number(number, sizeof number, (unsigned)(k / 8));
            int nw = kit_text_w(number, 0);
            if (labels && px - nw / 2 >= all.x && px + nw / 2 < all.x + all.w)
                kit_text(number, px - nw / 2, kit_ty(y, h - 1), TK_TEXT2, nw, 0);
        } else {
            int len = k % 4 == 0 ? 6 : k % 2 == 0 ? 4 : 2;
            rect(all, px, band_y + band_h - len, 1, len, TK_LINE_STRONG);
        }
    }
    for (int side = 0; side < 2; side++) {
        int mx = side ? right - 1 : left;
        for (int i = 0; i < 4; i++) rect(all, mx - 3 + i, band_y + i, 7 - 2 * i, 1, TK_ICON);
    }
}

/* The page scrolls on a tinted desk; its text, selection, find matches and
 * caret are clipped to the visible part of the page. */
static void draw_page(Clip all, const Frame *f) {
    Clip desk = intersect(all, (Clip){all.x, all.y + f->desk_y, f->sb_x, f->desk_h});
    rect(desk, desk.x, desk.y, desk.w, desk.h, TK_SUNKEN);
    int above = f->text_y - f->desk_y;
    int page_y = all.y + f->text_y - PAGE_PAD_V - state.scroll, page_x = all.x + f->page_x;
    int page_h = (state.content_height > state.viewport_height ? state.content_height : state.viewport_height) + 2 * PAGE_PAD_V;
    if (modern()) rect(desk, page_x + 1, page_y + page_h, f->page_w - 2, 2, TK_LINE_SUBTLE);
    else { rect(desk, page_x + f->page_w, page_y + 2, 2, page_h, TK_LINE); rect(desk, page_x + 2, page_y + page_h, f->page_w, 2, TK_LINE); }
    rect(desk, page_x, page_y, f->page_w, page_h, modern() ? TK_LINE_SUBTLE : TK_LINE);
    rect(desk, page_x + 1, page_y + 1, f->page_w - 2, page_h - 2, TK_RAISED);
    Clip content = intersect(desk, (Clip){page_x + 1, page_y + 1, f->page_w - 2, page_h - 2});
    if (!content.w || !content.h) return;
    const WriterDoc *d = document(); unsigned lo = selection_low(), hi = selection_high();
    uint8_t sel_fill = state.inactive ? TK_ACCENT_100 : TK_ACCENT, sel_ink = state.inactive ? TK_TEXT : TK_ON_ACCENT;
    unsigned needle = state.search.open ? string_length(state.search.text[0]) : 0, match_end = 0, start = 0;
    while (start + 1 < state.line_count && (int)(ARENA->lines[start].y + ARENA->lines[start].height) <= state.scroll - above) start++;
    /* Matches are found left to right, as Replace All finds them; catch up
     * on one that starts before the first visible line. */
    if (needle) {
        unsigned from = ARENA->lines[start].start;
        for (unsigned p = from >= needle ? from - needle + 1 : 0; p < from; p++)
            if (p >= match_end && search_match(p)) match_end = p + needle;
    }
    int origin_x = all.x + f->text_x, origin_y = all.y + f->text_y - state.scroll;
    for (unsigned row = start; row < state.line_count; row++) {
        const Line *line = &ARENA->lines[row];
        int py = origin_y + (int)line->y;
        if (py >= content.y + content.h) break;
        int base = origin_x + line_x(line), used = 0;
        for (unsigned p = line->start; p < line->end; p++) {
            int a = advance(d->text[p], d->style[p], line->paragraph, used);
            int selected = p >= lo && p < hi;
            if (needle && p >= match_end && search_match(p)) match_end = p + needle;
            if (selected) rect(content, base + used, py, a, line->height, sel_fill);
            else if (p < match_end) rect(content, base + used, py, a, line->height, TK_FIND);
            uint8_t ink = selected ? sel_ink : TK_TEXT;
            glyph(content, d->text[p], base + used, py + 2, d->style[p], line->paragraph, ink);
            if (d->style[p] & WRITER_STYLE_UNDERLINE)
                rect(content, base + used, py + scale_value(UI_HEIGHT, line->paragraph), a, 1, ink);
            used += a;
        }
        if (line->end < d->length && d->text[line->end] == '\n' && line->end >= lo && line->end < hi)
            rect(content, base + used, py, 5, line->height, sel_fill);
    }
    if (state.blink && !state.inactive && lo == hi && !(state.search.open && state.search.focus)) {
        int cx, cy, height; writer_position(snapshot()->caret, &cx, &cy, &height);
        rect(content, origin_x + cx, origin_y + cy + 2, 1, height - 4, TK_TEXT);
    }
}
/* Scrollbar thumb in content pixels; the track spans the desk. */
static int scroll_total(void) { return state.content_height > state.viewport_height ? state.content_height : state.viewport_height; }
static void scroll_thumb(const Frame *f, int *at, int *size) {
    kit_scroll_thumb(f->desk_h - 4, state.scroll, state.viewport_height, scroll_total(), at, size);
}
static void draw_scrollbar(Clip all, const Frame *f) {
    int sb = ui_theme.scrollbar;
    if (f->desk_h < 8 || !box_visible((Clip){f->sb_x, f->desk_y, sb, f->desk_h}, all.w, all.h)) return;
    int at, size, x = all.x + f->sb_x, y = all.y + f->desk_y; scroll_thumb(f, &at, &size);
    int st = state.dragging == 2 ? KIT_PRESSED : hot(x, y + 2 + at, sb, size);
    if (modern()) rect(all, x, y, sb, f->desk_h, TK_SUNKEN);
    kit_vscroll(x, y, f->desk_h, state.scroll, state.viewport_height, scroll_total(), st);
}

static void draw_status(Clip all, const Frame *f) {
    int sh = status_h(), sy = all.y + f->status_y, x = all.x, w = all.w;
    if (!box_visible((Clip){0, f->status_y, w, sh}, all.w, all.h)) return;
    if (modern()) {
        draw_hline(x, sy, w, TK_LINE_SUBTLE);
        kit_fill_bottom(x, sy + 1, w, sh - 1, ui_theme.r_window - 1, ui_theme.statusbar);
    } else kit_statusbar(x, sy, w, sh);
    int pad = modern() ? 12 : 8, ty = kit_ty(sy + 1, sh - 1);
    /* Position on the wrapped line and the message at the left; counts and
     * the save state at the right, dropped first when space runs out. */
    unsigned caret = snapshot()->caret, row = position_line(caret);
    if (row && snapshot()->affinity && ARENA->lines[row - 1].end == caret) row--;
    char where[40] = "Ln "; append_number(where, sizeof where, row + 1);
    unsigned n = string_length(where); text_copy(where + n, sizeof where - n, ", Col ");
    append_number(where, sizeof where, caret - ARENA->lines[row].start + 1);
    unsigned word_count = writer_word_count(), length = document()->length;
    char words[24] = "", chars[32] = "";
    append_number(words, sizeof words, word_count);
    n = string_length(words); text_copy(words + n, sizeof words - n, word_count == 1 ? " word" : " words");
    append_number(chars, sizeof chars, length);
    n = string_length(chars); text_copy(chars + n, sizeof chars - n, length == 1 ? " character" : " characters");
    int pending = state.save.pending, native = state.save.kind == DOCUMENT_SAVE_NATIVE;
    const char *saved = pending ? (native ? "Saving..." : "Exporting...") : writer_dirty() ? "Modified" : writer_file() >= 0 ? "Saved" : "";
    const char *message = pending ? (native ? "New edits stay private." : "Native state unchanged.") : state.status;
    const char *right_items[3] = {saved, chars, words};
    int left = x + pad, right = x + w - pad, where_w = kit_text_w(where, 0);
    if (left + where_w < right) { kit_text(where, left, ty, TK_TEXT2, where_w, 0); left += where_w + 16; }
    for (unsigned i = 0; i < 3; i++) {
        int iw = kit_text_w(right_items[i], 0);
        if (!iw) continue;
        if (right - iw < left + 80) break;
        kit_text(right_items[i], right - iw, ty, TK_TEXT2, iw, 0);
        right -= iw + 16;
    }
    kit_text(message, left, ty, TK_TEXT2, right - left, 0);
}

static void draw_toolbar(Clip all, const Frame *f) {
    int x = all.x, y = all.y, h = f->bar.height;
    rect(all, x, y, all.w, h - 1, modern() ? ui_theme.chrome : TK_BASE);
    rect(all, x, y + h - 1, all.w, 1, modern() ? TK_LINE : TK_LINE_SUBTLE);
    for (int i = 0; i < f->bar.dividers; i++) {
        Clip dv = f->bar.divider[i];
        if (box_visible(dv, all.w, all.h)) kit_vseparator(x + dv.x, y + dv.y, dv.h);
    }
    for (unsigned i = 0; i < TOOL_COUNT; i++) {
        Clip b = f->bar.item[i]; const ToolItem *it = &tool_items[i];
        if (!box_visible(b, all.w, all.h)) continue;
        int st = button_state(it->action);
        if (!(st & KIT_DISABLED)) st |= hot(x + b.x, y + b.y, b.w, b.h);
        tool_draw(x + b.x, y + b.y, b.w, b.h, it->icon, it->label, st);
    }
}

void writer_draw(int x, int y, int w, int h) {
    if (!state.initialized) writer_init();
    if (w <= 0 || h <= 0) return;
    Frame f; geometry(w, h, &f);
    Clip all = {x, y, w, h};
    draw_toolbar(all, &f);
    draw_ruler(all, &f);
    draw_page(all, &f);
    draw_scrollbar(all, &f);
    draw_find(all, &f);
    draw_status(all, &f);
}
int writer_click(int x, int y, int w, int h, int mx, int my, int modifiers) {
    if (!state.initialized) writer_init();
    if (mx < x || mx >= x + w || my < y || my >= y + h) return 0;
    Frame f; geometry(w, h, &f);
    int lx = mx - x, ly = my - y;
    int searched = find_click(&f, w, h, lx, ly, modifiers); if (searched) return searched;
    for (unsigned i = 0; i < TOOL_COUNT; i++) {
        Clip b = f.bar.item[i];
        if (box_visible(b, w, h) && lx >= b.x && lx < b.x + b.w && ly >= b.y && ly < b.y + b.h) {
            state.search.focus = 0;
            int result = button_action(tool_items[i].action); reveal(); return result;
        }
    }
    if (ly < f.desk_y || ly >= f.desk_y + f.desk_h) return 0;
    state.search.focus = 0; state.dragging = 0;
    if (lx >= f.sb_x) {
        /* Track clicks page; the thumb drags. */
        int range = state.content_height - state.viewport_height, at, size;
        state.manual_scroll = 1;
        if (range <= 0) return WRITER_CHANGED;
        scroll_thumb(&f, &at, &size);
        int offset = ly - f.desk_y - 2;
        if (offset >= at && offset < at + size) { state.dragging = 2; state.grab = offset - at; }
        else state.scroll = clamp(state.scroll + (offset < at ? -1 : 1) * state.viewport_height, 0, range);
        return WRITER_CHANGED;
    }
    move_hit(lx - f.text_x, ly - f.text_y + state.scroll, modifiers & WRITER_MOD_SHIFT, 0);
    state.dragging = 1; return WRITER_CHANGED;
}
int writer_drag(int x, int y, int w, int h, int mx, int my) {
    if (!state.dragging) return 0;
    Frame f; geometry(w, h, &f);
    int lx = mx - x, ly = my - y;
    if (state.dragging == 2) {
        int range = state.content_height - state.viewport_height, at, size;
        scroll_thumb(&f, &at, &size);
        int travel = f.desk_h - 4 - size;
        state.scroll = range > 0 && travel > 0 ? clamp((ly - f.desk_y - 2 - state.grab) * range / travel, 0, range) : 0;
        return WRITER_CHANGED;
    }
    if (ly < f.desk_y) writer_scroll(-1); else if (ly >= f.desk_y + f.desk_h) writer_scroll(1);
    move_hit(lx - f.text_x, ly - f.text_y + state.scroll, 1, 0);
    return WRITER_CHANGED;
}
