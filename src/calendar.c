/* Small month calendar and local day agenda. Storage owns accepted appointments
 * and the unfinished entry, so closing the window never discards either. */
#include "app.h"
#include "calendar.h"
#include "calendar_agenda.h"

#define ROWS 6
#define ROW_H 23
#define GRID_Y 86
#define GRID_H 28
#define KEY_DELETE 0x53
#define KEY_HOME 0x47
#define KEY_END 0x4f
#define KEY_PGUP 0x49
#define KEY_PGDN 0x51

enum { CONFIRM_NONE, CONFIRM_DELETE, CONFIRM_CANCEL };
static int view_year, view_month, view_day, inited;
static unsigned selected, confirming_id;
static int first_row, confirm, caret, select_all, entry_focus;
static const char *notice = "";

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
    if (row >= first_row + ROWS) first_row = row - ROWS + 1;
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
        focus_field(0); notice = "Use a real date: YYYY-MM-DD (1900-9999)."; return;
    }
    if (!cal_agenda_parse_time(d->time, &minute)) {
        focus_field(1); notice = "Use 24-hour HH:MM, or leave time blank for all-day."; return;
    }
    int nonspace = 0;
    for (int i = 0; d->title[i]; ++i) nonspace |= d->title[i] != ' ';
    if (!nonspace) { focus_field(2); notice = "Add an appointment title."; return; }
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
static void button(int x, int y, int w, const char *s, int active) {
    draw_round_rect(x, y, w, 26, 5, active ? app_accent : app_chrome_dk);
    if (!active) draw_round_rect(x + 1, y + 1, w - 2, 24, 4, COLOR_WHITE);
    draw_string(s, x + (w - ui_string_w(s)) / 2, y + 4, active ? COLOR_WHITE : app_text);
}
static void draw_nav(int x, int y, int dir) {
    button(x, y, 28, "", 0);
    int cx = x + 14, cy = y + 13;
    for (int i = 0; i < 5; i++) {
        int xx = dir < 0 ? cx - 2 + i : cx + 2 - i;
        draw_line(xx, cy - i, xx, cy + i, app_text);
    }
}
static void draw_field(int x, int y, int w, const char *s, int field, int active) {
    int focused = active && entry_focus && cal_agenda_draft()->field == field && !confirm;
    draw_round_rect(x, y, w, 28, 4, focused ? app_accent : app_chrome_dk);
    draw_round_rect(x + 1, y + 1, w - 2, 26, 3, COLOR_WHITE);
    int n = length(s), cursor = caret;
    if (cursor < 0) cursor = 0;
    if (cursor > n) cursor = n;
    int start = 0, width = 0;
    if (focused) {
        for (int i = 0; i < cursor; ++i) width += ui_advance(s[i]);
        while (start < cursor && width > w - 16) width -= ui_advance(s[start++]);
    }
    if (focused && select_all) draw_rect(x + 4, y + 4, w - 8, 20, app_accent);
    draw_string_clip(s + start, x + 6, y + 5,
                     focused && select_all ? COLOR_WHITE : active ? app_text : app_text_dim, x + w - 5);
    if (focused && !select_all) draw_vline(x + 6 + width, y + 5, 18, app_text);
}
void cal_draw(int bx, int by, int bw, int bh) {
    ensure_open();
    RtcTime t; rtc_read(&t);
    draw_rect(bx, by, bw, bh, COLOR_WHITE);
    if (bw < CAL_W || bh < CAL_H - 1) {
        draw_string_clip("Enlarge Calendar to see appointments.", bx + 12, by + 12, app_text, bx + bw - 8);
        return;
    }
    int left_w = bw / 2 - 6, right = bx + bw / 2 + 16, rw = bw - (right - bx) - 16;
    draw_nav(bx + 16, by + 14, -1); draw_nav(bx + left_w - 40, by + 14, 1);
    char title[32]; int n = 0;
    const char *m = rtc_month_name[view_month - 1];
    while (*m) title[n++] = *m++;
    title[n++] = ' '; char num[12]; fmt_uint(num, (unsigned)view_year);
    for (int i = 0; num[i]; ++i) title[n++] = num[i];
    title[n] = 0;
    draw_string_bold(title, bx + (left_w - uib_string_w(title)) / 2, by + 18, app_text);
    int gx = bx + 16, cw = (left_w - 32) / 7;
    static const char *dn[7] = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};
    for (int i = 0; i < 7; ++i)
        draw_string(dn[i], gx + i * cw + (cw - ui_string_w(dn[i])) / 2, by + 58, app_text_dim);
    draw_hline(gx, by + 80, cw * 7, app_chrome_dk);
    unsigned marked = 0;
    for (int i = 0; i < cal_agenda_count(); ++i) {
        const CalAppointment *a = cal_agenda_get(i);
        if (a->year == view_year && a->month == view_month) marked |= 1u << (a->day - 1);
    }
    int first = rtc_weekday(view_year, view_month, 1);
    for (int d = 1; d <= cal_agenda_days_in_month(view_year, view_month); ++d) {
        int index = first + d - 1, col = index % 7;
        int x = gx + col * cw, y = by + GRID_Y + index / 7 * GRID_H;
        int is_today = view_year == t.year && view_month == t.month && d == t.day;
        if (d == view_day) draw_round_frame(x + 2, y + 1, cw - 4, GRID_H - 2, 6, app_accent);
        if (is_today) draw_round_rect(x + (cw - 22) / 2, y + 2, 22, 22, 11, app_accent);
        char s[4]; fmt_uint(s, (unsigned)d);
        draw_string(s, x + (cw - ui_string_w(s)) / 2, y + 5,
                    is_today ? COLOR_WHITE : (col == 0 || col == 6) ? app_text_dim : app_text);
        if (marked & (1u << (d - 1))) draw_round_rect(x + cw / 2 - 2, y + 24, 4, 3, 1, app_accent);
    }
    button(bx + 16, by + 258, 66, "Today", 0);
    draw_string("Dots mark appointments", bx + 94, by + 262, app_text_dim);
    draw_vline(bx + bw / 2 + 3, by + 14, 268, app_chrome_dk);
    char date[11]; date_text(date, view_year, view_month, view_day);
    draw_string_bold(date, right, by + 16, app_text);
    int count = day_count(); fmt_uint(num, (unsigned)count);
    draw_string(num, right, by + 41, app_text_dim);
    draw_string(count == 1 ? "appointment" : "appointments", right + ui_string_w(num) + 5, by + 41, app_text_dim);
    if (first_row >= count) first_row = count > 0 ? count - 1 : 0;
    for (int row = 0; row < ROWS; ++row) {
        const CalAppointment *a = day_get(first_row + row);
        if (!a) break;
        int y = by + 70 + row * ROW_H, chosen = a->id == selected;
        if (chosen) draw_round_rect(right - 3, y, rw + 6, ROW_H - 1, 4, app_chrome);
        char time[6]; time_text(time, a->minute);
        draw_string(a->minute < 0 ? "All-day" : time, right + 3, y + 2, app_text_dim);
        draw_string_clip(a->title, right + 63, y + 2, app_text, right + rw - 4);
    }
    if (!count) draw_string("No appointments. Add one below.", right, by + 77, app_text_dim);
    draw_nav(right, by + 217, -1); draw_nav(right + rw - 28, by + 217, 1);
    char page[28]; int p = 0;
    fmt_uint(page, (unsigned)(count ? first_row + 1 : 0)); p = length(page); page[p++] = '-';
    fmt_uint(page + p, (unsigned)(first_row + ROWS < count ? first_row + ROWS : count)); p = length(page);
    copy(page + p, " of "); p += 4; fmt_uint(page + p, (unsigned)count);
    draw_string(page, right + (rw - ui_string_w(page)) / 2, by + 221, app_text_dim);
    button(right, by + 256, 76, "New", 0);
    button(right + 84, by + 256, 76, "Edit", 0);
    button(right + 168, by + 256, rw - 168, "Delete", 0);
    draw_hline(bx + 16, by + 289, bw - 32, app_chrome_dk);
    const CalAgendaDraft *d = cal_agenda_draft();
    if (confirm) {
        if (confirm == CONFIRM_DELETE) {
            const CalAppointment *a = cal_agenda_find(confirming_id);
            draw_string_bold_clip("Delete appointment?", bx + 16, by + 296, app_text, bx + bw - 180);
            draw_string_clip(a ? a->title : "Appointment unavailable", bx + 16, by + 323, app_text, bx + bw - 16);
        } else draw_string_bold_clip("Discard unfinished entry?", bx + 16, by + 296, app_text, bx + bw - 180);
        button(bx + bw - 170, by + 293, 74, confirm == CONFIRM_DELETE ? "Delete" : "Discard", 1);
        button(bx + bw - 88, by + 293, 72, "Keep", 0);
    } else {
        draw_string_bold(d->active ? d->edit_id ? "Edit appointment" : "New appointment" : "Select a day, then New", bx + 16, by + 296, app_text);
        if (d->active) {
            button(bx + bw - 170, by + 293, 74, "Save", 1);
            button(bx + bw - 88, by + 293, 72, "Cancel", 0);
        }
    }
    if (!confirm) {
        draw_string("Date (YYYY-MM-DD)", bx + 16, by + 324, app_text_dim);
        draw_string("Time (HH:MM)", bx + 188, by + 324, app_text_dim);
    }
    draw_field(bx + 16, by + 345, 156, d->date, 0, d->active);
    draw_field(bx + 188, by + 345, 100, d->time, 1, d->active);
    button(bx + 300, by + 346, 82, "All-day", d->active && !d->time[0]);
    draw_string("Title", bx + 16, by + 393, app_text_dim);
    draw_field(bx + 60, by + 387, bw - 76, d->title, 2, d->active);
    const char *hint = confirm ? "Enter confirms. Escape keeps your work." :
        d->active ? "Tab: fields   Ctrl+A: replace   Enter: save   Esc: close, keep entry" :
        "Arrows: day / appointment   PgUp/PgDn: month   N: new   Enter: edit";
    draw_string_clip(notice[0] ? notice : hint, bx + 16, by + 424, app_text_dim, bx + bw - 16);
    draw_string_clip(cal_agenda_status(), bx + 16, by + 450, app_text_dim, bx + bw - 102);
    button(bx + bw - 88, by + 444, 72, "Retry", 0);
}
int cal_click(int bx, int by, int bw, int mx, int my) {
    ensure_open();
    if (bw < CAL_W) return 0;
    if (hit(mx, my, bx + bw - 88, by + 444, 72, 26)) {
        cal_agenda_retry_save(); return CAL_CHANGED;
    }
    if (confirm) {
        if (hit(mx, my, bx + bw - 170, by + 293, 74, 26)) accept_confirmation();
        else if (hit(mx, my, bx + bw - 88, by + 293, 72, 26)) confirm = CONFIRM_NONE;
        return CAL_CHANGED;
    }
    int left_w = bw / 2 - 6, right = bx + bw / 2 + 16, rw = bw - (right - bx) - 16;
    if (hit(mx, my, bx + 16, by + 14, 28, 26)) { move_month(-1); return CAL_CHANGED; }
    if (hit(mx, my, bx + left_w - 40, by + 14, 28, 26)) { move_month(1); return CAL_CHANGED; }
    if (hit(mx, my, bx + 16, by + 258, 66, 26)) { today(); return CAL_CHANGED; }
    int gx = bx + 16, cw = (left_w - 32) / 7;
    if (hit(mx, my, gx, by + GRID_Y, cw * 7, GRID_H * 6)) {
        int day = (my - by - GRID_Y) / GRID_H * 7 + (mx - gx) / cw - rtc_weekday(view_year, view_month, 1) + 1;
        if (cal_agenda_valid_date(view_year, view_month, day)) { view_day = day; changed_day(); }
        return CAL_CHANGED;
    }
    if (hit(mx, my, right, by + 70, rw, ROWS * ROW_H)) {
        const CalAppointment *a = day_get(first_row + (my - by - 70) / ROW_H);
        if (a) { selected = a->id; entry_focus = 0; notice = ""; }
        return CAL_CHANGED;
    }
    if (hit(mx, my, right, by + 217, 28, 26)) { first_row = first_row > ROWS ? first_row - ROWS : 0; select_row(first_row); entry_focus = 0; return CAL_CHANGED; }
    if (hit(mx, my, right + rw - 28, by + 217, 28, 26)) { if (first_row + ROWS < day_count()) first_row += ROWS; select_row(first_row); entry_focus = 0; return CAL_CHANGED; }
    if (hit(mx, my, right, by + 256, 76, 26)) return cal_new();
    if (hit(mx, my, right + 84, by + 256, 76, 26)) { edit_selected(); return CAL_CHANGED; }
    if (hit(mx, my, right + 168, by + 256, rw - 168, 26)) { ask_delete(); return CAL_CHANGED; }
    if (cal_agenda_draft()->active) {
        if (hit(mx, my, bx + bw - 170, by + 293, 74, 26)) { save_entry(); return CAL_CHANGED; }
        if (hit(mx, my, bx + bw - 88, by + 293, 72, 26)) { confirm = CONFIRM_CANCEL; return CAL_CHANGED; }
        if (hit(mx, my, bx + 16, by + 345, 156, 28)) { focus_field(0); return CAL_CHANGED; }
        if (hit(mx, my, bx + 188, by + 345, 100, 28)) { focus_field(1); return CAL_CHANGED; }
        if (hit(mx, my, bx + 60, by + 387, bw - 76, 28)) { focus_field(2); return CAL_CHANGED; }
        if (hit(mx, my, bx + 300, by + 346, 82, 26)) {
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
