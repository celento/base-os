/* Small month calendar and local day agenda. Storage owns accepted appointments
 * and the unfinished entry, so closing the window never discards either. */
#include "ui/app.h"
#include "apps/calendar.h"
#include "apps/calendar_agenda.h"
#include "apps/pane_chrome.h"

#define KEY_DELETE 0x53
#define KEY_HOME 0x47
#define KEY_END 0x4f
#define KEY_PGUP 0x49
#define KEY_PGDN 0x51

enum { CONFIRM_NONE, CONFIRM_DELETE, CONFIRM_CANCEL };
static int view_year, view_month, view_day, inited;
static unsigned selected, confirming_id;
static int first_row, confirm, caret, select_all, entry_focus;
/* Agenda rows on screen, from the last layout; keyboard paging uses it. */
static int page_rows = 6;
static const char *notice = "";
/* The field a failed save points at; it shows the error state while its
 * message is the notice. */
static const char *error_notice = "";
static int error_field = -1;

static int length(const char *s) { int n = 0; while (s[n]) ++n; return n; }
static void copy(char *d, const char *s) { while ((*d++ = *s++)) {} }
static void date_text(char *s, int y, int m, int d) {
    s[0] = '0' + y / 1000; s[1] = '0' + y / 100 % 10;
    s[2] = '0' + y / 10 % 10; s[3] = '0' + y % 10; s[4] = '-';
    s[5] = '0' + m / 10; s[6] = '0' + m % 10; s[7] = '-';
    s[8] = '0' + d / 10; s[9] = '0' + d % 10; s[10] = 0;
}
static void time_text(char *s, int minute) {
    if (minute < 0) { s[0] = 0; return; }
    s[0] = '0' + minute / 60 / 10; s[1] = '0' + minute / 60 % 10;
    s[2] = ':'; s[3] = '0' + minute % 60 / 10;
    s[4] = '0' + minute % 10; s[5] = 0;
}
static char *field_text(CalAgendaDraft *d) {
    return d->field == 0 ? d->date : d->field == 1 ? d->time : d->title;
}
static void focus_field(int field) {
    CalAgendaDraft d = *cal_agenda_draft();
    if (!d.active) return;
    d.field = field;
    cal_agenda_set_draft(&d);
    caret = length(field_text(&d)); select_all = 0; entry_focus = 1;
    confirm = CONFIRM_NONE; notice = "";
}
static int on_day(const CalAppointment *a, int day) {
    return a && a->year == view_year && a->month == view_month && a->day == day;
}
static int day_count(void) {
    int n = 0;
    for (int i = 0; i < cal_agenda_count(); ++i) n += on_day(cal_agenda_get(i), view_day);
    return n;
}
static const CalAppointment *day_get(int row) {
    for (int i = 0; i < cal_agenda_count(); ++i) {
        const CalAppointment *a = cal_agenda_get(i);
        if (on_day(a, view_day) && row-- == 0) return a;
    }
    return 0;
}
static int selected_row(void) {
    for (int i = 0; i < day_count(); ++i) if (day_get(i)->id == selected) return i;
    return -1;
}
static void select_row(int row) {
    int n = day_count();
    if (!n) { selected = 0; first_row = 0; return; }
    if (row < 0) row = 0;
    if (row >= n) row = n - 1;
    selected = day_get(row)->id;
    if (row < first_row) first_row = row;
    if (row >= first_row + page_rows) first_row = row - page_rows + 1;
}
static void changed_day(void) {
    selected = 0; first_row = 0; confirm = CONFIRM_NONE; entry_focus = 0;
    select_all = 0; notice = ""; select_row(0);
}
void cal_reset(void) {
    cal_agenda_load();
    if (!inited) {
        RtcTime t; rtc_read(&t);
        view_year = t.year; view_month = t.month; view_day = t.day;
        if (!cal_agenda_valid_date(view_year, view_month, view_day)) {
            view_year = 2000; view_month = 1; view_day = 1;
        }
        const CalAgendaDraft *d = cal_agenda_draft();
        int y, m, day;
        if (d->active && cal_agenda_parse_date(d->date, &y, &m, &day)) {
            view_year = y; view_month = m; view_day = day;
        }
        inited = 1;
    }
    confirm = CONFIRM_NONE; notice = ""; select_all = 0;
    if (cal_agenda_draft()->active) focus_field(cal_agenda_draft()->field);
    else entry_focus = 0;
    if (selected_row() < 0) select_row(0);
}
static void ensure_open(void) { if (!inited) cal_reset(); }
static void move_month(int direction) {
    if (cal_agenda_step_month(&view_year, &view_month, &view_day, direction)) changed_day();
}
static void today(void) {
    RtcTime t; rtc_read(&t);
    if (cal_agenda_valid_date(t.year, t.month, t.day)) {
        view_year = t.year; view_month = t.month; view_day = t.day; changed_day();
    }
}
int cal_new(void) {
    ensure_open();
    if (cal_agenda_draft()->active) {
        focus_field(cal_agenda_draft()->field);
        notice = "Finish or cancel the current entry first.";
        return CAL_CHANGED;
    }
    CalAgendaDraft d = {0};
    d.active = 1; d.field = 2;
    date_text(d.date, view_year, view_month, view_day);
    cal_agenda_set_draft(&d); focus_field(2);
    return CAL_CHANGED;
}
static void edit_selected(void) {
    if (cal_agenda_draft()->active) { (void)cal_new(); return; }
    const CalAppointment *a = cal_agenda_find(selected);
    if (!on_day(a, view_day)) { notice = "Choose an appointment to edit."; return; }
    CalAgendaDraft d = {0};
    d.active = 1; d.edit_id = a->id; d.field = 2;
    date_text(d.date, a->year, a->month, a->day); time_text(d.time, a->minute);
    copy(d.title, a->title);
    cal_agenda_set_draft(&d); focus_field(2);
}
static void save_entry(void) {
    const CalAgendaDraft *d = cal_agenda_draft();
    int y, m, day, minute;
    if (!d->active) return;
    if (!cal_agenda_parse_date(d->date, &y, &m, &day)) {
        focus_field(0); error_field = 0; notice = error_notice = "Use a real date: YYYY-MM-DD (1900-9999)."; return;
    }
    if (!cal_agenda_parse_time(d->time, &minute)) {
        focus_field(1); error_field = 1; notice = error_notice = "Use 24-hour HH:MM, or leave time blank for all-day."; return;
    }
    int nonspace = 0;
    for (int i = 0; d->title[i]; ++i) nonspace |= d->title[i] != ' ';
    if (!nonspace) { focus_field(2); error_field = 2; notice = error_notice = "Add an appointment title."; return; }
    unsigned id = cal_agenda_commit_draft();
    if (!id) { notice = "Entry kept. List full or edited appointment unavailable."; return; }
    view_year = y; view_month = m; view_day = day;
    changed_day(); selected = id; select_row(selected_row());
}
static void ask_delete(void) {
    const CalAppointment *a = cal_agenda_find(selected);
    if (!on_day(a, view_day)) { notice = "Choose an appointment to delete."; return; }
    if (cal_agenda_draft()->active) {
        notice = "Finish or cancel the current entry before deleting."; return;
    }
    confirming_id = a->id; confirm = CONFIRM_DELETE; notice = "";
}
static void accept_confirmation(void) {
    if (confirm == CONFIRM_CANCEL) {
        cal_agenda_clear_draft(); entry_focus = select_all = 0;
    } else if (confirm == CONFIRM_DELETE) {
        int row = selected_row();
        if (!cal_agenda_delete(confirming_id)) notice = "Appointment was not deleted.";
        else select_row(row);
    }
    confirm = CONFIRM_NONE;
}
/* Layout: a toolbar (month navigation, Today, New / Edit / Delete, Save),
 * a sidebar with the month grid, the day's agenda on the right with the
 * entry form under it, and a status bar. Drawing and hit testing share
 * cal_layout. */
enum { CB_PREV_MONTH, CB_NEXT_MONTH, CB_TODAY, CB_NEW, CB_EDIT, CB_DELETE, CB_RETRY,
       CB_PAGE_UP, CB_PAGE_DOWN, CB_DISMISS, CB_ACCEPT, CB_DATE, CB_TIME, CB_ALL_DAY,
       CB_TITLE, CB_COUNT };
typedef PaneRect CalRect;
typedef struct {
    CalRect r[CB_COUNT];
    int body_y, status_y, side_w;
    int gx, gy, cw, ch;        /* month grid cells */
    int pane_x, pane_w;        /* right pane, inside the frame */
    int px, pw;                /* right pane content column */
    int ly, rows;              /* agenda rows */
    int form_y;                /* rule above the entry form */
} CalLayout;
static void cal_layout(int bx, int by, int bw, int bh, CalLayout *L) {
    int modern = PANE_MODERN, pad = pane_pad(), h = ui_theme.control_h;
    int cy = pane_ctl_y(by), x = bx + pad, right = bx + bw - pad, w;
    CalRect *r = L->r;
    L->body_y = pane_body_y(by);
    L->status_y = pane_status_y(by, bh);
    r[CB_PREV_MONTH] = (CalRect){x, cy, h, h}; x += h + 2;
    r[CB_NEXT_MONTH] = (CalRect){x, cy, h, h}; x += h + 8;
    r[CB_TODAY] = (CalRect){x, cy, pane_tool_w("Today"), h};
    w = pane_tool_w("Save"); r[CB_RETRY] = (CalRect){right - w, cy, w, h}; right -= w + 13;
    w = pane_tool_w("Delete"); r[CB_DELETE] = (CalRect){right - w, cy, w, h}; right -= w + 4;
    w = pane_tool_w("Edit"); r[CB_EDIT] = (CalRect){right - w, cy, w, h}; right -= w + 4;
    w = pane_tool_w("New"); r[CB_NEW] = (CalRect){right - w, cy, w, h};
    /* Month grid in the sidebar. */
    int sp = modern ? 16 : 12;
    L->side_w = modern ? 264 : 252;
    L->cw = (L->side_w - 2 * sp) / 7;
    L->ch = modern ? 30 : 26;
    L->gx = bx + (L->side_w - 7 * L->cw) / 2;
    L->gy = L->body_y + 62;
    /* Agenda pane. */
    int rp = modern ? 16 : 12;
    L->pane_x = bx + L->side_w + 1;
    L->pane_w = bx + bw - L->pane_x;
    L->px = L->pane_x + rp;
    L->pw = bx + bw - rp - L->px;
    int hy = L->body_y + 12;   /* two header lines: date, count */
    r[CB_PAGE_DOWN] = (CalRect){L->px + L->pw - h, hy + (2 * UI_FONT_H + 3 - h) / 2, h, h};
    r[CB_PAGE_UP] = (CalRect){r[CB_PAGE_DOWN].x - h - 2, r[CB_PAGE_DOWN].y, h, h};
    L->ly = hy + 2 * UI_FONT_H + 3 + 8;
    /* Entry form: heading with Cancel / Save, Date and Time with labels,
     * then Title with its label at the left. */
    L->form_y = L->status_y - (41 + 3 * h + (modern ? 16 : 10));
    int y1 = L->form_y + 8, y2 = y1 + h + 6 + UI_FONT_H + 2, y3 = y2 + h + 8;
    w = kit_button_w("Save", -1);
    int dw = kit_button_w("Discard", -1);
    if (dw > w) w = dw;
    r[CB_ACCEPT] = (CalRect){L->px + L->pw - w, y1, w, h};
    int kw = kit_button_w("Cancel", -1);
    r[CB_DISMISS] = (CalRect){r[CB_ACCEPT].x - 8 - kw, y1, kw, h};
    r[CB_DATE] = (CalRect){L->px, y2, 116, h};
    r[CB_TIME] = (CalRect){L->px + 124, y2, 72, h};
    r[CB_ALL_DAY] = (CalRect){L->px + 204, y2, pane_tool_w("All-day"), h};
    int lw = kit_text_w("Title", 0) + 10;
    r[CB_TITLE] = (CalRect){L->px + lw, y3, L->pw - lw, h};
    L->rows = (L->form_y - 4 - L->ly) / ui_theme.row_h;
    if (L->rows < 1) L->rows = 1;
}
static int cal_hit(int mx, int my, const CalRect *r) { return pane_hit(mx, my, r); }
static int field_state(int field, int active) {
    if (!active) return KIT_DISABLED;
    int error = notice[0] && notice == error_notice && error_field == field ? KIT_ERROR : 0;
    return (entry_focus && cal_agenda_draft()->field == field && !confirm ? KIT_FOCUS : 0) | error;
}
static void draw_field(const CalRect *r, const char *s, const char *placeholder, int field, int active) {
    int state = field_state(field, active), focused = state & KIT_FOCUS;
    int tx = kit_field(r->x, r->y, r->w, r->h, state), ty = kit_ty(r->y, r->h);
    int room = r->x + r->w - (PANE_MODERN ? 10 : 6) - tx;
    int n = length(s), cursor = caret;
    if (cursor < 0) cursor = 0;
    if (cursor > n) cursor = n;
    int start = 0, width = 0;
    if (focused) {
        for (int i = 0; i < cursor; ++i) width += ui_advance(s[i]);
        while (start < cursor && width > room - 2) width -= ui_advance(s[start++]);
    }
    if (!s[0]) {
        if (active && !focused) kit_text(placeholder, tx, ty, TK_TEXT2, room, 0);
    } else if (focused && select_all) {
        int sw = kit_text_w(s + start, 0);
        draw_rect(tx - 1, ty, (sw < room ? sw : room) + 2, UI_FONT_H, TK_ACCENT);
        kit_text(s + start, tx, ty, TK_ON_ACCENT, room, 0);
    } else kit_text(s + start, tx, ty, active ? TK_TEXT : TK_TEXT2, room, 0);
    if (focused && !select_all) kit_caret(tx + width, ty);
}
static void long_date(char *s, int y, int m, int d) {
    char num[12];
    pane_join(s, 40, rtc_day_name[rtc_weekday(y, m, d)], ", ", rtc_month_name[m - 1]);
    fmt_uint(num, (unsigned)d);
    pane_append(s, 40, " "); pane_append(s, 40, num); pane_append(s, 40, ", ");
    fmt_uint(num, (unsigned)y);
    pane_append(s, 40, num);
}
static void draw_month(const CalLayout *L, int bx, const RtcTime *t) {
    int modern = PANE_MODERN, cw = L->cw, ch = L->ch;
    char title[32], num[12];
    fmt_uint(num, (unsigned)view_year);
    pane_join(title, sizeof title, rtc_month_name[view_month - 1], " ", num);
    kit_text(title, L->gx + 4, L->body_y + 12, TK_TEXT, 7 * cw - 8, 1);
    static const char *dn[7] = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};
    for (int i = 0; i < 7; ++i)
        kit_text_center(dn[i], L->gx + i * cw, L->body_y + 40, cw, TK_TEXT2, 0);
    kit_separator(L->gx, L->gy - 4, 7 * cw);
    unsigned marked = 0;
    for (int i = 0; i < cal_agenda_count(); ++i) {
        const CalAppointment *a = cal_agenda_get(i);
        if (a->year == view_year && a->month == view_month) marked |= 1u << (a->day - 1);
    }
    int first = rtc_weekday(view_year, view_month, 1);
    for (int d = 1; d <= cal_agenda_days_in_month(view_year, view_month); ++d) {
        int index = first + d - 1, col = index % 7;
        int x = L->gx + col * cw, y = L->gy + index / 7 * ch;
        int is_today = view_year == t->year && view_month == t->month && d == t->day;
        int chosen = d == view_day;
        int fx = x + 2, fy = y + 1, fw = cw - 4, fh = ch - 2;
        if (chosen) kit_fill(fx, fy, fw, fh, modern ? 6 : KIT_R_SQUARE, TK_ACCENT, 1);
        if (is_today) {
            kit_frame(fx, fy, fw, fh, modern ? 6 : KIT_R_SQUARE, chosen ? TK_ACCENT_700 : TK_ACCENT);
            if (chosen) kit_frame(fx + 1, fy + 1, fw - 2, fh - 2, modern ? 5 : KIT_R_SQUARE, TK_ON_ACCENT);
        }
        char s[4]; fmt_uint(s, (unsigned)d);
        uint8_t ink = chosen ? TK_ON_ACCENT : is_today ? TK_ACCENT_700 :
                      (col == 0 || col == 6) ? TK_TEXT2 : TK_TEXT;
        kit_text_center(s, x, kit_ty(y, ch) - 1, cw, ink, is_today || chosen);
        if (marked & (1u << (d - 1)))
            kit_fill(x + cw / 2 - 2, y + ch - 6, 4, 3, modern ? 1 : KIT_R_SQUARE, chosen ? TK_ON_ACCENT : TK_ACCENT, 0);
    }
    /* Legend. */
    int ly = L->gy + 6 * ch + 14;
    kit_fill(L->gx + 8, ly + 7, 4, 3, modern ? 1 : KIT_R_SQUARE, TK_ACCENT, 0);
    kit_text("Has appointments", L->gx + 22, ly, TK_TEXT2, 7 * cw - 22, 0);
    kit_frame(L->gx + 4, ly + 22, 12, 12, modern ? 3 : KIT_R_SQUARE, TK_ACCENT);
    kit_text("Today", L->gx + 22, ly + 20, TK_TEXT2, 7 * cw - 22, 0);
    (void)bx;
}
static void draw_agenda(const CalLayout *L, PaneHot hot) {
    int count = day_count(), rh = ui_theme.row_h, hy = L->body_y + 12;
    char head[40], num[12], line[48];
    long_date(head, view_year, view_month, view_day);
    kit_text(head, L->px, hy, TK_TEXT, L->r[CB_PAGE_UP].x - 8 - L->px, 1);
    fmt_uint(num, (unsigned)count);
    pane_join(line, sizeof line, num, count == 1 ? " appointment" : " appointments", 0);
    kit_text(line, L->px, hy + UI_FONT_H + 3, TK_TEXT2, L->pw / 2, 0);
    if (first_row >= count) first_row = count > 0 ? count - 1 : 0;
    if (count > L->rows) {
        char page[28]; int p;
        fmt_uint(page, (unsigned)(first_row + 1)); p = length(page); page[p++] = '-';
        fmt_uint(page + p, (unsigned)(first_row + L->rows < count ? first_row + L->rows : count)); p = length(page);
        copy(page + p, " of "); p += 4; fmt_uint(page + p, (unsigned)count);
        kit_text_right(page, L->r[CB_PAGE_UP].x - 8, hy + UI_FONT_H + 3, TK_TEXT2, 0);
    }
    int up = first_row > 0, down = first_row + L->rows < count;
    const CalRect *pu = &L->r[CB_PAGE_UP], *pd = &L->r[CB_PAGE_DOWN];
    pane_tool(pu, ICON_CHEVRON_UP, 0, KB_FLAT, up ? pane_hot(hot, pu->x, pu->y, pu->w, pu->h) : KIT_DISABLED);
    pane_tool(pd, ICON_CHEVRON_DOWN, 0, KB_FLAT, down ? pane_hot(hot, pd->x, pd->y, pd->w, pd->h) : KIT_DISABLED);
    for (int row = 0; row < L->rows; ++row) {
        const CalAppointment *a = day_get(first_row + row);
        if (!a) break;
        int y = L->ly + row * rh, chosen = a->id == selected;
        int state = chosen ? KIT_ON | (entry_focus ? KIT_UNFOCUSED : 0)
                           : pane_hot(hot, L->pane_x, y, L->pane_w, rh);
        uint8_t ink = kit_row(L->pane_x, y, L->pane_w, rh, state), dim = kit_row_dim(state);
        char time[6]; time_text(time, a->minute);
        int ty = kit_ty(y, rh);
        kit_text(a->minute < 0 ? "All-day" : time, L->px, ty, dim, 60, 0);
        kit_text(a->title, L->px + 66, ty, ink, L->pw - 66, 0);
    }
    if (!count) {
        int ey = L->ly + (L->form_y - L->ly) / 2 - UI_FONT_H;
        kit_text_center("No appointments", L->px, ey, L->pw, TK_TEXT, 1);
        kit_text_center("Choose New to add one to this day.", L->px, ey + UI_FONT_H + 4, L->pw, TK_TEXT2, 0);
    }
}
static void draw_form(const CalLayout *L, PaneHot hot) {
    const CalAgendaDraft *d = cal_agenda_draft();
    const CalRect *r = L->r, *ok = &r[CB_ACCEPT], *no = &r[CB_DISMISS];
    kit_separator(L->pane_x, L->form_y, L->pane_w);
    int ty = kit_ty(ok->y, ok->h);
    int ok_state = pane_hot(hot, ok->x, ok->y, ok->w, ok->h) | KIT_FOCUS;
    int no_state = pane_hot(hot, no->x, no->y, no->w, no->h);
    if (confirm) {
        const CalAppointment *a = cal_agenda_find(confirming_id);
        int del = confirm == CONFIRM_DELETE;
        kit_text(del ? "Delete appointment?" : "Discard unfinished entry?", L->px, ty, TK_TEXT, no->x - 8 - L->px, 1);
        kit_button(no->x, no->y, no->w, no->h, "Keep", -1, KB_DEFAULT, no_state);
        kit_button(ok->x, ok->y, ok->w, ok->h, del ? "Delete" : "Discard", -1, KB_DANGER, ok_state);
        const char *what = del ? (a ? a->title : "Appointment unavailable") : d->title[0] ? d->title : "Untitled entry";
        char text[CAL_AGENDA_TITLE_MAX + 48];
        pane_join(text, sizeof text, del ? "\"" : "Your changes to \"", what,
                  del ? "\" will be removed." : "\" will be lost.");
        kit_banner(L->px, r[CB_DATE].y - UI_FONT_H - 2, L->pw, KIT_WARN, text);
        return;
    }
    kit_text(d->active ? d->edit_id ? "Edit appointment" : "New appointment" : "Select a day, then New",
             L->px, ty, d->active ? TK_TEXT : TK_TEXT2, (d->active ? no->x - 8 : L->px + L->pw) - L->px, 1);
    if (d->active) {
        kit_button(no->x, no->y, no->w, no->h, "Cancel", -1, KB_DEFAULT, no_state);
        kit_button(ok->x, ok->y, ok->w, ok->h, "Save", -1, KB_PRIMARY, ok_state);
    }
    int label_y = r[CB_DATE].y - UI_FONT_H - 2;
    kit_text("Date", r[CB_DATE].x, label_y, TK_TEXT2, r[CB_DATE].w, 0);
    kit_text("Time", r[CB_TIME].x, label_y, TK_TEXT2, r[CB_TIME].w, 0);
    draw_field(&r[CB_DATE], d->date, "YYYY-MM-DD", 0, d->active);
    draw_field(&r[CB_TIME], d->time, "HH:MM", 1, d->active);
    const CalRect *all = &r[CB_ALL_DAY];
    pane_tool(all, ICON_CALENDAR, "All-day", KB_DEFAULT,
              !d->active ? KIT_DISABLED : (!d->time[0] ? KIT_ON : 0) | pane_hot(hot, all->x, all->y, all->w, all->h));
    kit_text("Title", L->px, kit_ty(r[CB_TITLE].y, r[CB_TITLE].h), TK_TEXT2, r[CB_TITLE].x - L->px, 0);
    draw_field(&r[CB_TITLE], d->title, "What is happening", 2, d->active);
}
void cal_draw(int bx, int by, int bw, int bh, PaneHot hot) {
    ensure_open();
    RtcTime t; rtc_read(&t);
    draw_rect(bx, by, bw, bh, ui_theme.content);
    if (bw < CAL_W || bh < CAL_H) {
        kit_text("Enlarge Calendar to see appointments.", bx + 12, by + 12, TK_TEXT2, bw - 20, 0);
        return;
    }
    CalLayout L; cal_layout(bx, by, bw, bh, &L);
    page_rows = L.rows;
    CalRect *r = L.r;
    /* Toolbar */
    pane_toolbar(bx, by, bw);
    static const int icons[] = {ICON_CHEVRON_LEFT, ICON_CHEVRON_RIGHT, ICON_TODAY, ICON_CALENDAR_PLUS,
                                ICON_EDIT, ICON_TRASH, ICON_SAVE};
    static const char *const labels[] = {0, 0, "Today", "New", "Edit", "Delete", "Save"};
    int have = on_day(cal_agenda_find(selected), view_day);
    for (int i = CB_PREV_MONTH; i <= CB_RETRY; ++i) {
        int state = (i == CB_EDIT || i == CB_DELETE) && !have ? KIT_DISABLED
                  : pane_hot(hot, r[i].x, r[i].y, r[i].w, r[i].h);
        pane_tool(&r[i], icons[i], labels[i], KB_DEFAULT, state);
    }
    kit_vseparator(r[CB_RETRY].x - 7, r[CB_RETRY].y + 4, r[CB_RETRY].h - 8);
    /* Sidebar */
    draw_rect(bx, L.body_y, L.side_w, L.status_y - L.body_y, ui_theme.sidebar);
    kit_vseparator(bx + L.side_w, L.body_y, L.status_y - L.body_y);
    draw_month(&L, bx, &t);
    draw_agenda(&L, hot);
    draw_form(&L, hot);
    /* Status bar */
    const CalAgendaDraft *d = cal_agenda_draft();
    const char *hint = confirm ? "Enter confirms; Esc keeps your work" :
        d->active ? "Tab: next field  Enter: save  Esc: close, keep entry" :
        "Arrows: day, item   PgUp/PgDn: month   N: new";
    pane_statusbar(bx, by, bw, bh, notice[0] ? notice : hint, notice[0] ? TK_RED_700 : TK_TEXT2,
                   cal_agenda_status(), TK_TEXT2);
}
int cal_click(int bx, int by, int bw, int bh, int mx, int my) {
    ensure_open();
    if (bw < CAL_W || bh < CAL_H) return 0;
    CalLayout L; cal_layout(bx, by, bw, bh, &L);
    page_rows = L.rows;
    CalRect *r = L.r;
    if (cal_hit(mx, my, &r[CB_RETRY])) {
        cal_agenda_retry_save(); return CAL_CHANGED;
    }
    if (confirm) {
        if (cal_hit(mx, my, &r[CB_ACCEPT])) accept_confirmation();
        else if (cal_hit(mx, my, &r[CB_DISMISS])) confirm = CONFIRM_NONE;
        return CAL_CHANGED;
    }
    if (cal_hit(mx, my, &r[CB_PREV_MONTH])) { move_month(-1); return CAL_CHANGED; }
    if (cal_hit(mx, my, &r[CB_NEXT_MONTH])) { move_month(1); return CAL_CHANGED; }
    if (cal_hit(mx, my, &r[CB_TODAY])) { today(); return CAL_CHANGED; }
    if (hit(mx, my, L.gx, L.gy, L.cw * 7, L.ch * 6)) {
        int day = (my - L.gy) / L.ch * 7 + (mx - L.gx) / L.cw - rtc_weekday(view_year, view_month, 1) + 1;
        if (cal_agenda_valid_date(view_year, view_month, day)) { view_day = day; changed_day(); }
        return CAL_CHANGED;
    }
    if (hit(mx, my, L.pane_x, L.ly, L.pane_w, L.rows * ui_theme.row_h)) {
        const CalAppointment *a = day_get(first_row + (my - L.ly) / ui_theme.row_h);
        if (a) { selected = a->id; entry_focus = 0; notice = ""; }
        return CAL_CHANGED;
    }
    if (cal_hit(mx, my, &r[CB_PAGE_UP])) { first_row = first_row > page_rows ? first_row - page_rows : 0; select_row(first_row); entry_focus = 0; return CAL_CHANGED; }
    if (cal_hit(mx, my, &r[CB_PAGE_DOWN])) { if (first_row + page_rows < day_count()) first_row += page_rows; select_row(first_row); entry_focus = 0; return CAL_CHANGED; }
    if (cal_hit(mx, my, &r[CB_NEW])) return cal_new();
    if (cal_hit(mx, my, &r[CB_EDIT])) { edit_selected(); return CAL_CHANGED; }
    if (cal_hit(mx, my, &r[CB_DELETE])) { ask_delete(); return CAL_CHANGED; }
    if (cal_agenda_draft()->active) {
        if (cal_hit(mx, my, &r[CB_ACCEPT])) { save_entry(); return CAL_CHANGED; }
        if (cal_hit(mx, my, &r[CB_DISMISS])) { confirm = CONFIRM_CANCEL; return CAL_CHANGED; }
        if (cal_hit(mx, my, &r[CB_DATE])) { focus_field(0); return CAL_CHANGED; }
        if (cal_hit(mx, my, &r[CB_TIME])) { focus_field(1); return CAL_CHANGED; }
        if (cal_hit(mx, my, &r[CB_TITLE])) { focus_field(2); return CAL_CHANGED; }
        if (cal_hit(mx, my, &r[CB_ALL_DAY])) {
            CalAgendaDraft d = *cal_agenda_draft(); d.time[0] = 0;
            cal_agenda_set_draft(&d); focus_field(1); return CAL_CHANGED;
        }
    }
    return 0;
}
int cal_key(int sc, char ch, int modifiers) {
    ensure_open();
    if (modifiers & CAL_MOD_ALT) return 0;
    if (modifiers & CAL_MOD_CTRL) {
        if (sc == 0x1f) { cal_agenda_retry_save(); return CAL_CHANGED; }
        if (sc == 0x31) return cal_new();
        if (sc == 0x1e && cal_agenda_draft()->active && entry_focus && !confirm) {
            select_all = 1; return CAL_CHANGED;
        }
        return 0;
    }
    if (confirm) {
        if (sc == KEY_ENTER) accept_confirmation();
        else if (sc == KEY_ESC) confirm = CONFIRM_NONE;
        return CAL_CHANGED;
    }
    if (sc == KEY_ESC) return CAL_CLOSE;
    CalAgendaDraft d = *cal_agenda_draft();
    if (sc == KEY_TAB && d.active) {
        focus_field((d.field + ((modifiers & CAL_MOD_SHIFT) ? 2 : 1)) % 3); return CAL_CHANGED;
    }
    if (d.active && entry_focus) {
        char *s = field_text(&d); int n = length(s), cap = d.field == 0 ? 10 : d.field == 1 ? 5 : CAL_AGENDA_TITLE_MAX;
        if (caret > n) caret = n;
        if (sc == KEY_ENTER) { save_entry(); return CAL_CHANGED; }
        if (sc == KEY_LEFT || sc == KEY_RIGHT || sc == KEY_HOME || sc == KEY_END) {
            if (sc == KEY_HOME) caret = 0;
            else if (sc == KEY_END) caret = n;
            else if (sc == KEY_LEFT && caret > 0) --caret;
            else if (sc == KEY_RIGHT && caret < n) ++caret;
            select_all = 0; return CAL_CHANGED;
        }
        if (sc == KEY_BACKSPACE || sc == KEY_DELETE || (ch >= ' ' && ch <= '~')) {
            if (select_all) { s[0] = 0; n = caret = 0; select_all = 0; }
            if (sc == KEY_BACKSPACE && caret > 0) {
                for (int i = caret - 1; i < n; ++i) s[i] = s[i + 1];
                --caret;
            } else if (sc == KEY_DELETE && caret < n) {
                for (int i = caret; i < n; ++i) s[i] = s[i + 1];
            } else if (ch >= ' ' && ch <= '~' && n < cap) {
                for (int i = n + 1; i > caret; --i) s[i] = s[i - 1];
                s[caret++] = ch;
            }
            cal_agenda_set_draft(&d); notice = ""; return CAL_CHANGED;
        }
        return 0;
    }
    if (sc == KEY_LEFT || sc == KEY_RIGHT) {
        if (cal_agenda_step_day(&view_year, &view_month, &view_day, sc == KEY_LEFT ? -1 : 1)) changed_day();
    } else if (sc == KEY_UP || sc == KEY_DOWN) {
        select_row(selected_row() + (sc == KEY_UP ? -1 : 1));
    } else if (sc == KEY_PGUP || sc == KEY_PGDN) move_month(sc == KEY_PGUP ? -1 : 1);
    else if (sc == KEY_HOME) today();
    else if (sc == KEY_ENTER) edit_selected();
    else if (sc == KEY_DELETE) ask_delete();
    else if (ch == 'n' || ch == 'N') return cal_new();
    else return 0;
    return CAL_CHANGED;
}
