#include "apps/player.h"
#include "kernel/audio.h"
#include "lib/video.h"
#include "ui/app.h"
#include "kernel/fs.h"
#include "kernel/kheap.h"
#include "kernel/platform.h"
#include "ui/icon_draw.h"
#include "ui/icons.h"
#include "ui/kit.h"
#include "ui/ui_icons.h"

static int *files, file_capacity, file_count, selected, first_visible, visible_rows = 4;
static unsigned *identities;      /* files and identities hold file_capacity entries */
static int source_id = -1;
static unsigned source_identity;
static char title[FS_NAME_LEN], message[96];
static unsigned refresh_time, last_position, last_volume;
static int last_state, last_error, initialized, video_mode;
static int redraw_pending;
static AudioStatus last_audio;
static unsigned last_audio_capacity;

static void copy_text(char *to, const char *from, unsigned size) {
    if (!size) return;
    unsigned i = 0;
    while (i + 1 < size && from[i]) { to[i] = from[i]; ++i; }
    to[i] = 0;
}
static void append(char *to, const char *from, unsigned size) {
    unsigned n = 0;
    while (n < size && to[n]) ++n;
    if (n < size) copy_text(to + n, from, size - n);
}
static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 'a' - 'A') : c; }
static int extension(const char *name, const char *suffix) {
    unsigned n = 0, len = 0;
    while (name[n]) ++n;
    while (suffix[len]) ++len;
    if (n < len) return 0;
    for (unsigned i = 0; i < len; ++i) if (lower(name[n-len+i]) != suffix[i]) return 0;
    return 1;
}
static int is_video(int id) {
    const char *name = fs_name(id);
    return extension(name, ".mpg") || extension(name, ".mpeg");
}
static int is_media(int id) {
    const char *name = fs_name(id);
    return extension(name, ".wav") || extension(name, ".wave") || extension(name, ".mp3") || is_video(id);
}
static void keep_selection_visible(void) {
    if (selected < 0) { first_visible = 0; return; }
    if (selected < first_visible) first_visible = selected;
    if (selected >= first_visible + visible_rows) first_visible = selected - visible_rows + 1;
    int last = file_count - visible_rows;
    if (first_visible > last) first_visible = last;
    if (first_visible < 0) first_visible = 0;
}
void player_refresh(void) {
    redraw_pending = 1;
    int old_id = selected >= 0 && selected < file_count ? files[selected] : -1;
    unsigned old_identity = selected >= 0 && selected < file_count ? identities[selected] : 0;
    file_count = 0; selected = -1;
    if (fs_node_count() > file_capacity) {
        int *more = kmalloc((uint32_t)fs_node_count() * sizeof *more);
        unsigned *more_identities = kmalloc((uint32_t)fs_node_count() * sizeof *more_identities);
        if (more && more_identities) {
            kfree(files); kfree(identities);
            files = more; identities = more_identities; file_capacity = fs_node_count();
        } else {
            kfree(more); kfree(more_identities);
        }
    }
    int trash = fs_find_child(fs_root(), "trash");
    for (int id = 0; id < fs_node_slots() && file_count < file_capacity; ++id) {
        if (!fs_valid(id) || fs_is_dir(id) || fs_is_app(id) || !is_media(id)) continue;
        int parent = fs_parent(id), excluded = 0;
        for (int depth = 0; depth <= FS_MAX_DEPTH && parent > 0; ++depth) {
            if (parent == trash) { excluded = 1; break; }
            parent = fs_parent(parent);
        }
        if (excluded) continue;
        files[file_count] = id; identities[file_count] = fs_identity(id);
        if (id == old_id && identities[file_count] == old_identity) selected = file_count;
        ++file_count;
    }
    /* Stable alphabetical listing, independent of sparse filesystem slots. */
    for (int i = 1; i < file_count; ++i) {
        int id = files[i]; unsigned identity = identities[i]; int j = i;
        while (j && kstrcmp(fs_name(files[j-1]), fs_name(id)) > 0) {
            files[j] = files[j-1]; identities[j] = identities[j-1]; --j;
        }
        files[j] = id; identities[j] = identity;
    }
    selected = -1;
    for (int i = 0; i < file_count; ++i)
        if (files[i] == old_id && identities[i] == old_identity) selected = i;
    if (selected < 0 && file_count) selected = 0;
    keep_selection_visible(); refresh_time = timer_ticks();
}
void player_init(void) {
    if (initialized) return;
    initialized = 1; selected = -1; source_id = -1;
    title[0] = message[0] = 0;
    player_refresh();
    if (!audio_status()->available) copy_text(message, media_error_string(MEDIA_NO_DEVICE), sizeof message);
    last_state = -1; last_error = 0;
}
int player_open_file(int id) {
    player_init();
    redraw_pending = 1;
    if (!fs_valid(id) || fs_is_dir(id) || fs_is_app(id)) {
        copy_text(message, "This media file is no longer available.", sizeof message);
        return MEDIA_BAD_FILE;
    }
    int bytes = fs_size(id);
    unsigned capacity = is_video(id) ? VIDEO_MAX_FILE_BYTES : audio_capacity_bytes();
    if (bytes <= 0 || (unsigned)bytes > capacity) {
        int error = bytes <= 0 ? MEDIA_BAD_FILE : MEDIA_TOO_LARGE;
        copy_text(message, media_error_string(error), sizeof message); return error;
    }
    int opening_video = is_video(id);
    int error = opening_video ? video_play(fs_data(id), (unsigned)bytes) : audio_play(fs_data(id), (unsigned)bytes);
    if (error) { copy_text(message, opening_video ? video_error_string(error) : media_error_string(error), sizeof message); return error; }
    if (opening_video) audio_stop(); else video_stop();
    video_mode = opening_video;
    source_id = id; source_identity = fs_identity(id);
    copy_text(title, fs_name(id), sizeof title); message[0] = 0;
    player_refresh();
    for (int i = 0; i < file_count; ++i) if (files[i] == id) selected = i;
    keep_selection_visible(); return MEDIA_OK;
}
static int open_selected(void) {
    redraw_pending = 1;
    if (selected < 0 || selected >= file_count) {
        copy_text(message, "Add a WAV, MP3 or MPEG-1 file, then choose Play.", sizeof message); return 1;
    }
    if (!fs_valid(files[selected]) || fs_identity(files[selected]) != identities[selected]) {
        player_refresh(); copy_text(message, "The selected file changed. Choose it again.", sizeof message); return 1;
    }
    player_open_file(files[selected]); return 1;
}
static int play_pause(void) {
    if (video_mode) {
        int state = video_status()->state;
        if (state == VIDEO_PLAYING || state == VIDEO_LOADING) { video_pause(1); return 1; }
        if (state == VIDEO_PAUSED) { video_pause(0); return 1; }
    }
    const AudioStatus *status = audio_status();
    if (!video_mode && status->state == AUDIO_PLAYING) { audio_pause(1); return 1; }
    if (!video_mode && status->state == AUDIO_PAUSED) { audio_pause(0); return 1; }
    if (!video_mode && status->state == AUDIO_LOADING) return 0;
    if (selected >= 0 && selected < file_count) return open_selected();
    if (source_id >= 0 && fs_valid(source_id) && fs_identity(source_id) == source_identity) {
        player_open_file(source_id); return 1;
    }
    redraw_pending=1;
    copy_text(message, "Choose a media file from the list below.", sizeof message); return 1;
}
static void time_string(char *out, unsigned milliseconds) {
    unsigned seconds = milliseconds / 1000;
    fmt_uint(out, seconds / 60);
    unsigned n = (unsigned)kstrlen(out); out[n++] = ':';
    fmt_pad2(out+n, (int)(seconds % 60));
    if ((video_mode ? video_duration_ms() : audio_duration_ms()) < 10000) {
        n += 2; out[n++] = '.'; out[n++] = (char)('0' + milliseconds/100%10); out[n] = 0;
    }
}
static const char *state_string(int state) {
    switch (state) {
    case AUDIO_LOADING: return "Buffering";
    case AUDIO_PLAYING: return "Playing";
    case AUDIO_PAUSED: return "Paused";
    case AUDIO_FINISHED: return "Finished";
    case AUDIO_ERROR: return "Stopped";
    default: return "Ready";
    }
}
/* Layout, shared by drawing, the compositor fast paths and hit testing.
 * Audio: a Now Playing card (art, title, format, state and the progress
 * strip), the transport row (Play, Stop, Clear error, volume, Refresh), an
 * optional message, the media library and a status bar. Video replaces the
 * card with a title row, the viewport and the progress strip. */
#define MODERN (ui_theme.style == UI_STYLE_MODERN)
#define SB (ui_theme.scrollbar)
enum { PB_PLAY, PB_STOP, PB_CLEAR, PB_REFRESH, PB_COUNT };
typedef struct { int x, y, w, h; } PlayerRect;
typedef struct {
    int ok, video, divider, divider2, row_h, rows_y;
    PlayerRect card, art, heading, viewport, strip, b[PB_COUNT], volume, message;
    PlayerRect library, up, down, list, scroll, status;
} PlayerLayout;
#define PLAY_D 40
#define TRANSPORT_H 40
#define STRIP_H (18 + UI_FONT_H)
#define VOL_TEXT_W 40
static int side_pad(void) { return MODERN ? 16 : 12; }
static int status_h(void) { return MODERN ? 28 : 24; }
static int list_inset(void) { return MODERN ? 4 : 1; }
static int message_h(void) { return MODERN ? 36 : 32; }
static int volume_shown(void) { return !video_mode || video_status()->audio_enabled; }
static void player_layout(int x, int y, int w, int h, PlayerLayout *L) {
    *L = (PlayerLayout){0};
    L->ok = w >= 420 && h >= 362;
    L->video = video_mode;
    int pad = side_pad(), gap = 12, cs = ui_theme.control_h, cx = x + pad, cw = w - 2 * pad, cy = y + pad;
    L->row_h = ui_theme.row_h;
    L->status = (PlayerRect){x, y + h - status_h(), w, status_h()};
    int msg = message[0] ? message_h() + 8 : 0, lib_h = cs + 6;
    if (!video_mode) {
        L->card = (PlayerRect){cx, cy, cw, 16 + 64 + 12 + STRIP_H + 12};
        L->art = (PlayerRect){cx + 16, cy + 16, 64, 64};
        L->strip = (PlayerRect){cx + 16, cy + 16 + 64 + 12, cw - 32, STRIP_H};
        cy += L->card.h + gap;
    } else {
        L->heading = (PlayerRect){cx, cy, cw, 2 * UI_FONT_H + 6};
        cy += L->heading.h + 8;
        /* The viewport takes two thirds of what the fixed rows leave. */
        int avail = L->status.y - gap - cy - (8 + STRIP_H + gap) - (TRANSPORT_H + gap) - msg - lib_h;
        int list_h = avail / 3, min_list = 2 * L->row_h + 2 * list_inset();
        if (list_h < min_list) list_h = min_list;
        int vh = avail - list_h;
        if (vh < 68) vh = 68;
        L->viewport = (PlayerRect){cx, cy, cw, vh};
        cy += vh + 8;
        L->strip = (PlayerRect){cx, cy, cw, STRIP_H};
        cy += STRIP_H + gap;
    }
    /* Transport */
    int by = cy + (TRANSPORT_H - cs) / 2;
    L->b[PB_PLAY] = (PlayerRect){cx, cy, PLAY_D, PLAY_D};
    L->b[PB_STOP] = (PlayerRect){cx + PLAY_D + 10, by, cs, cs};
    L->divider = L->b[PB_STOP].x + cs + 8;
    L->b[PB_CLEAR] = (PlayerRect){L->divider + 9, by, 12 + 16 + 6 + ui_string_w("Clear error") + 12, cs};
    L->b[PB_REFRESH] = (PlayerRect){cx + cw - cs, by, cs, cs};
    L->divider2 = L->b[PB_REFRESH].x - 9;
    /* Volume: speaker, slider and percentage, when there is room. */
    int left = L->b[PB_CLEAR].x + L->b[PB_CLEAR].w + 16 + 22, right = L->divider2 - 9 - VOL_TEXT_W;
    if (volume_shown() && right - left >= 48) L->volume = (PlayerRect){left, cy + (TRANSPORT_H - 18) / 2, right - left, 18};
    cy += TRANSPORT_H + gap;
    if (msg) { L->message = (PlayerRect){cx, cy, cw, message_h()}; cy += msg; }
    /* Library */
    L->library = (PlayerRect){cx, cy, cw, lib_h};
    L->down = (PlayerRect){cx + cw - cs, cy + (lib_h - cs) / 2, cs, cs};
    L->up = (PlayerRect){L->down.x - cs - 4, L->down.y, cs, cs};
    cy += lib_h;
    L->list = (PlayerRect){cx, cy, cw, L->status.y - gap - cy};
    if (L->list.h < 0) L->list.h = 0;
    L->rows_y = L->list.y + list_inset();
    visible_rows = (L->list.h - 2 * list_inset()) / L->row_h;
    if (visible_rows < 1) visible_rows = 1;
    if (file_count > visible_rows)
        L->scroll = (PlayerRect){L->list.x + L->list.w - 1 - SB, L->list.y + 1, SB, L->list.h - 2};
}
/* The rectangle of visible library row k (0 = first shown), or w 0. */
static PlayerRect row_rect(const PlayerLayout *L, int k) {
    PlayerRect r = {L->list.x + 1, L->rows_y + k * L->row_h, L->list.w - 2 - (L->scroll.w ? SB : 0), L->row_h};
    if (r.y + r.h > L->list.y + L->list.h - list_inset()) r.w = 0;
    return r;
}

/* The desktop passes its hover tracker while the window is active, so
 * pointer motion over a control repaints it. Without one nothing hovers. */
static int (*hover_fn)(int x, int y, int w, int h);
void player_set_hover(int (*fn)(int x, int y, int w, int h)) { hover_fn = fn; }
static int hover(PlayerRect r) { return hover_fn && r.w > 0 ? hover_fn(r.x, r.y, r.w, r.h) : 0; }

/* A bordered tool with a toolbar glyph and an optional label. */
static void tool(PlayerRect r, int glyph, const char *label, int state) {
    int dis = state & KIT_DISABLED;
    kit_button(r.x, r.y, r.w, r.h, "", -1, KB_DEFAULT, state);
    uint8_t ink = dis ? TK_LINE_STRONG : TK_ICON;
    int dy = (state & KIT_PRESSED) && !dis && !MODERN ? 1 : 0, iy = r.y + (r.h - ICON_SIZE) / 2 + dy;
    if (label) {
        int content = ICON_SIZE + 6 + ui_string_w(label), ix = r.x + (r.w - content) / 2;
        icon_draw(glyph, ix, iy, ink);
        kit_text(label, ix + ICON_SIZE + 6, kit_ty(r.y, r.h) + dy, dis ? TK_LINE_STRONG : TK_TEXT, r.x + r.w - 4 - ix - ICON_SIZE - 6, 0);
    } else icon_draw(glyph, r.x + (r.w - ICON_SIZE) / 2, iy, ink);
}
/* A flat tool: no fill until hovered. */
static void flat_tool(PlayerRect r, int glyph, int state) {
    int dis = state & KIT_DISABLED;
    if (!dis && (state & (KIT_HOVER | KIT_PRESSED)))
        kit_fill(r.x, r.y, r.w, r.h, MODERN ? ui_theme.r_control : KIT_R_SQUARE, (state & KIT_PRESSED) ? TK_LINE_SUBTLE : TK_HOVER, 1);
    icon_draw(glyph, r.x + (r.w - ICON_SIZE) / 2, r.y + (r.h - ICON_SIZE) / 2, dis ? (MODERN ? TK_LINE_STRONG : TK_LINE) : TK_ICON);
}
static void play_button(PlayerRect r, int playing, int enabled) {
    int state = enabled ? hover(r) : KIT_DISABLED;
    uint8_t fill = !enabled ? (MODERN ? TK_HOVER : TK_BASE) : (state & KIT_PRESSED) ? TK_ACCENT_700 :
        (state & KIT_HOVER) ? TK_ACCENT_600 : TK_ACCENT;
    if (MODERN) kit_fill(r.x, r.y, r.w, r.h, r.w / 2, fill, 1);
    else kit_box(r.x, r.y, r.w, r.h, KIT_R_NOTCH, fill, enabled ? TK_ACCENT_700 : TK_LINE_SUBTLE);
    /* The play triangle sits 1 px right of centre so it looks optically centred. */
    icon_draw(playing ? ICON_PAUSE : ICON_PLAY, r.x + (r.w - ICON_SIZE) / 2 + (playing ? 0 : 1), r.y + (r.h - ICON_SIZE) / 2,
              enabled ? TK_ON_ACCENT : TK_LINE_STRONG);
}
static void draw_transport(const PlayerLayout *L, int playing, int play_enabled, int error) {
    play_button(L->b[PB_PLAY], playing, play_enabled);
    tool(L->b[PB_STOP], ICON_STOP_SQUARE, 0, hover(L->b[PB_STOP]));
    kit_vseparator(L->divider, L->b[PB_STOP].y + 5, L->b[PB_STOP].h - 10);
    tool(L->b[PB_CLEAR], ICON_CLEAR, "Clear error", error ? hover(L->b[PB_CLEAR]) : KIT_DISABLED);
    tool(L->b[PB_REFRESH], ICON_REFRESH, 0, hover(L->b[PB_REFRESH]));
    if (L->volume.w) {
        char text[8];
        unsigned volume = audio_status()->volume;
        PlayerRect v = L->volume;
        icon_draw(ICON_VOLUME, v.x - 22, v.y + (v.h - ICON_SIZE) / 2, TK_ICON);
        kit_slider(v.x, v.y + (MODERN ? 1 : 0), v.w, (int)volume, 100, hover(v));
        fmt_uint(text, volume); append(text, "%", sizeof text);
        kit_text_right(text, v.x + v.w + 6 + VOL_TEXT_W, kit_ty(v.y, v.h), TK_TEXT, 0);
        kit_vseparator(L->divider2, L->b[PB_REFRESH].y + 5, L->b[PB_REFRESH].h - 10);
    }
}
/* Volume from a pointer x on the slider, matching kit_slider's travel. */
static unsigned volume_at(const PlayerLayout *L, int mx) {
    int knob = MODERN ? 16 : 12, travel = L->volume.w - knob;
    int v = travel > 0 ? (mx - L->volume.x - knob / 2) * 100 / travel : 0;
    return (unsigned)(v < 0 ? 0 : v > 100 ? 100 : v);
}
static int hit_rect(int mx, int my, PlayerRect r) { return r.w > 0 && hit(mx, my, r.x, r.y, r.w, r.h); }
/* The progress strip: bar and times on an opaque background, so the fast
 * path can repaint it alone and match a full draw pixel for pixel. */
static void draw_strip(PlayerRect s, unsigned position, unsigned duration, uint8_t bg) {
    char text[32];
    draw_rect(s.x, s.y, s.w, s.h, bg);
    unsigned scale = duration / 65535u + 1;
    unsigned progress = duration ? (position / scale) * (unsigned)s.w / (duration / scale) : 0;
    if (progress > (unsigned)s.w) progress = (unsigned)s.w;
    kit_progress(s.x, s.y + (MODERN ? 2 : 0), s.w, (int)progress, s.w);
    time_string(text, position); kit_text(text, s.x, s.y + 18, TK_TEXT2, s.w / 2, 0);
    time_string(text, duration); kit_text_right(text, s.x + s.w, s.y + 18, TK_TEXT2, 0);
}
/* A one-line notice above the library: amber, or red for a device or
 * decoder error. */
static void draw_message(PlayerRect m, int error) {
    uint8_t fill = error ? TK_RED_50 : TK_AMBER_50;
    if (MODERN) kit_box(m.x, m.y, m.w, m.h, 8, fill, 0);
    else kit_box(m.x, m.y, m.w, m.h, KIT_R_SQUARE, fill, error ? TK_RED : TK_AMBER);
    ui_icon_draw(error ? UI_ICON_ERR : UI_ICON_WARN, m.x + 12, m.y + (m.h - 16) / 2, 16, TK_ICON, fill);
    kit_text(message, m.x + 12 + 16 + 8, kit_ty(m.y, m.h), TK_TEXT, m.w - 12 - 16 - 8 - 12, 0);
}
/* A state pill at the right of a title row; returns its x. */
static int state_pill(int right, int y, const char *state, int active) {
    int pw = kit_text_w(state, 1) + 18, ph = MODERN ? 22 : 20;
    kit_fill(right - pw, y, pw, ph, MODERN ? 11 : KIT_R_SQUARE, active ? TK_ACCENT_100 : (MODERN ? TK_HOVER : TK_BASE), 1);
    kit_text(state, right - pw + 9, kit_ty(y, ph), active ? TK_ACCENT_700 : TK_TEXT2, pw, 1);
    return right - pw;
}
static const char *format_name(int id) {
    return is_video(id) ? "MPEG" : extension(fs_name(id), ".mp3") ? "MP3" : "WAV";
}
static void draw_library(const PlayerLayout *L) {
    char text[32], number[12];
    PlayerRect lib = L->library;
    int ty = kit_ty(lib.y, lib.h);
    int tw = kit_text(video_mode ? "Library" : "Media files", lib.x, ty, TK_TEXT, lib.w / 2, 1);
    fmt_uint(number, (unsigned)file_count);
    copy_text(text, number, sizeof text); append(text, file_count == 1 ? " file" : " files", sizeof text);
    kit_text(text, lib.x + tw + 10, ty, TK_TEXT2, L->up.x - lib.x - tw - 18, 0);
    /* The arrows move the selection, exactly like the Up/Down keys. */
    flat_tool(L->up, ICON_CHEVRON_UP, selected > 0 ? hover(L->up) : KIT_DISABLED);
    flat_tool(L->down, ICON_CHEVRON_DOWN, selected + 1 < file_count ? hover(L->down) : KIT_DISABLED);
    PlayerRect list = L->list;
    if (list.h < 8) return;
    if (MODERN) kit_box(list.x, list.y, list.w, list.h, 8, TK_RAISED, TK_LINE_SUBTLE);
    else kit_box(list.x, list.y, list.w, list.h, KIT_R_SQUARE, TK_RAISED, TK_LINE);
    keep_selection_visible();
    if (!file_count) {
        int need = 32 + 12 + 2 * UI_FONT_H + 4, ey = list.y + (list.h - need) / 2;
        if (ey >= list.y + 4) {
            ui_icon_draw(UI_ICON_A_MUSIC, list.x + (list.w - 32) / 2, ey, 32, TK_ICON, TK_RAISED);
            kit_text_center("No media files", list.x + 8, ey + 44, list.w - 16, TK_TEXT, 1);
            kit_text_center("Import a WAV, MP3 or MPEG-1 file to begin.", list.x + 8, ey + 44 + UI_FONT_H + 4, list.w - 16, TK_TEXT2, 0);
        } else if (list.h >= UI_FONT_H + 4)
            kit_text("No media files yet.", list.x + 10, kit_ty(list.y, list.h), TK_TEXT2, list.w - 20, 0);
        return;
    }
    for (int k = 0; k < visible_rows && first_visible + k < file_count; ++k) {
        PlayerRect r = row_rect(L, k);
        if (!r.w) break;
        int index = first_visible + k, id = files[index];
        int state = index == selected ? KIT_ON : hover(r) & KIT_HOVER;
        uint8_t ink = kit_row(r.x, r.y, r.w, r.h, state), dim = kit_row_dim(state);
        uint8_t paper = (state & KIT_ON) ? TK_ACCENT : (state & KIT_HOVER) ? (MODERN ? TK_SUNKEN : TK_ACCENT_50) : TK_RAISED;
        int current = id == source_id && fs_identity(id) == source_identity, ix = r.x + (MODERN ? 14 : 8);
        if (current) icon_draw(ICON_PLAY, ix, r.y + (r.h - ICON_SIZE) / 2, (state & KIT_ON) ? TK_ON_ACCENT : TK_ACCENT);
        else ui_icon_draw(is_video(id) ? UI_ICON_SCREEN : UI_ICON_A_MUSIC, ix, r.y + (r.h - 16) / 2, 16,
                          (state & KIT_ON) ? TK_ON_ACCENT : TK_ICON, paper);
        const char *format = format_name(id);
        int fx = r.x + r.w - (MODERN ? 14 : 8) - kit_text_w(format, 0);
        kit_text(fs_name(id), ix + 24, kit_ty(r.y, r.h), ink, fx - ix - 24 - 12, current);
        kit_text(format, fx, kit_ty(r.y, r.h), dim, r.w, 0);
    }
    if (L->scroll.w) kit_vscroll(L->scroll.x, L->scroll.y, L->scroll.h, first_visible, visible_rows, file_count, hover(L->scroll));
}
static void draw_status(const PlayerLayout *L, const char *left, const char *right) {
    PlayerRect s = L->status;
    if (MODERN) {
        draw_hline(s.x, s.y, s.w, TK_LINE_SUBTLE);
        kit_fill_bottom(s.x, s.y + 1, s.w, s.h - 1, ui_theme.r_window > 1 ? ui_theme.r_window - 1 : 0, ui_theme.statusbar);
    } else kit_statusbar(s.x, s.y, s.w, s.h);
    int ty = kit_ty(s.y + 1, s.h - 1), pad = MODERN ? 12 : 8;
    int lw = kit_text(left, s.x + pad, ty, TK_TEXT2, s.w - 2 * pad, 0), rw = kit_text_w(right, 0);
    if (rw <= s.w - 3 * pad - lw) kit_text(right, s.x + s.w - pad - rw, ty, TK_TEXT2, rw, 0);
}
void player_draw_audio_progress(int x, int y, int w, int h) {
    if (video_mode) return;
    PlayerLayout L; player_layout(x, y, w, h, &L);
    if (!L.ok) return;
    draw_strip(L.strip, audio_position_ms(), audio_duration_ms(), TK_RAISED);
}
void player_draw_playback(int x,int y,int w,int h) {
    if (!video_mode) return;
    PlayerLayout L; player_layout(x, y, w, h, &L);
    if (!L.ok) return;
    video_draw(L.viewport.x, L.viewport.y, L.viewport.w, L.viewport.h);
    draw_strip(L.strip, video_position_ms(), video_duration_ms(), ui_theme.content);
}
static void draw_video(const PlayerLayout *L) {
    const VideoStatus *status = video_status();
    PlayerRect hd = L->heading;
    int px = state_pill(hd.x + hd.w, hd.y, state_string(status->state), status->state == VIDEO_PLAYING);
    kit_text(title[0] ? title : "Choose a video", hd.x, hd.y, TK_TEXT, px - hd.x - 12, 1);
    char text[96], number[20];
    if (status->audio_enabled) {
        copy_text(text,"MPEG-1 + MP2 / ",sizeof text);
        append(text,status->audio_channels==1?"Mono / ":"Stereo / ",sizeof text);
        fmt_uint(number,status->audio_sample_rate);append(text,number,sizeof text);append(text," Hz",sizeof text);
    } else copy_text(text,status->audio_error==MEDIA_NO_DEVICE?
        "MPEG-1 / Video only (no audio device)":"MPEG-1 / Video only (no audio)",sizeof text);
    if (status->width) {
        append(text, " / ", sizeof text); fmt_uint(number,status->width); append(text,number,sizeof text);
        append(text," x ",sizeof text); fmt_uint(number,status->height); append(text,number,sizeof text);
    }
    kit_text(text, hd.x, hd.y + UI_FONT_H + 6, TK_TEXT2, hd.w, 0);
    video_draw(L->viewport.x, L->viewport.y, L->viewport.w, L->viewport.h);
    draw_strip(L->strip, video_position_ms(), video_duration_ms(), ui_theme.content);
    draw_transport(L, status->state==VIDEO_PLAYING || status->state==VIDEO_LOADING, 1, status->error || message[0]);
    if (L->message.w) draw_message(L->message, status->error != 0);
    draw_library(L);
    const char *hint = status->audio_enabled ? "Space: play/pause   Enter: open   +/-: volume   S: stop" :
        status->audio_error == MEDIA_UNSUPPORTED ? "Unsupported MPEG audio; playing video only" :
        "Space: play/pause   Enter: open   S: stop";
    draw_status(L, status->audio_enabled ? "MPEG-1 video + MP2 audio / Sound Blaster 16 / 2 MiB" :
        "MPEG-1 video / 2 MiB / up to 640 x 480 / no audio", hint);
}
void player_draw(int x, int y, int w, int h) {
    player_init();
    draw_rect(x, y, w, h, ui_theme.content);
    PlayerLayout L; player_layout(x, y, w, h, &L);
    if (!L.ok) {
        if (w >= 200 && h >= 40) kit_text("Enlarge Media Player to show its controls.", x+12, y+12, TK_TEXT2, w-24, 0);
        return;
    }
    if (video_mode) { draw_video(&L); return; }
    const AudioStatus *status = audio_status();
    PlayerRect c = L.card, a = L.art;
    if (MODERN) kit_box(c.x, c.y, c.w, c.h, 10, TK_RAISED, TK_LINE_SUBTLE);
    else kit_box(c.x, c.y, c.w, c.h, KIT_R_SQUARE, TK_RAISED, TK_LINE_SUBTLE);
    kit_fill(a.x, a.y, a.w, a.h, MODERN ? 8 : KIT_R_SQUARE, TK_ACCENT_50, 1);
    ui_icon_draw(UI_ICON_A_MUSIC, a.x + 16, a.y + 16, 32, TK_ICON, TK_ACCENT_50);
    int tx = a.x + a.w + 16, right = c.x + c.w - 16;
    int px = state_pill(right, a.y + 2, state_string(status->state), status->state == AUDIO_PLAYING);
    kit_text(title[0] ? title : "Choose a track", tx, a.y + 4, TK_TEXT, px - tx - 12, 1);
    char text[96] = {0}, number[20];
    if (title[0]) {
        copy_text(text, status->format==AUDIO_FORMAT_MP3 ? "MP3" : "PCM WAV", sizeof text);
        append(text, status->channels==2 ? " / Stereo / " : " / Mono / ", sizeof text);
        fmt_uint(number, status->sample_rate); append(text, number, sizeof text); append(text, " Hz", sizeof text);
    } else copy_text(text, "Your music, played directly by BaseOS.", sizeof text);
    kit_text(text, tx, a.y + 4 + UI_FONT_H + 4, TK_TEXT2, right - tx, 0);
    kit_text(status->available ? "Sound Blaster 16" : "No audio device", tx, a.y + 4 + 2 * (UI_FONT_H + 4),
             status->available ? TK_TEXT2 : TK_AMBER_700, right - tx, 0);
    draw_strip(L.strip, audio_position_ms(), audio_duration_ms(), TK_RAISED);
    draw_transport(&L, status->state==AUDIO_PLAYING || status->state==AUDIO_LOADING,
        status->state!=AUDIO_LOADING, status->error || message[0]);
    if (L.message.w) draw_message(L.message, status->error != 0);
    draw_library(&L);
    copy_text(text, "WAV + MP3 / ", sizeof text); fmt_uint(number, audio_capacity_bytes()/1024);
    append(text, number, sizeof text); append(text, " KiB playback limit", sizeof text);
    draw_status(&L, text, "Space: play/pause   Enter: open   +/-: volume");
}
int player_click(int x, int y, int w, int h, int mx, int my) {
    player_init();
    PlayerLayout L; player_layout(x, y, w, h, &L);
    if (!L.ok) return 0;
    if (hit_rect(mx,my,L.b[PB_PLAY])) return play_pause();
    if (hit_rect(mx,my,L.b[PB_STOP])) { if (video_mode) video_stop(); else audio_stop(); return 1; }
    if (hit_rect(mx,my,L.b[PB_CLEAR])) {
        redraw_pending=1; message[0]=0;
        if (video_mode) video_clear_error(); else audio_clear_error();
        return 1;
    }
    if (hit_rect(mx,my,L.b[PB_REFRESH])) { player_refresh(); return 1; }
    if (L.volume.w && hit(mx,my,L.volume.x-4,L.volume.y-6,L.volume.w+8,L.volume.h+12)) {
        audio_set_volume(volume_at(&L, mx)); return 1;
    }
    if (hit_rect(mx,my,L.up)) return player_key(KEY_UP,0);
    if (hit_rect(mx,my,L.down)) return player_key(KEY_DOWN,0);
    /* The scrollbar track selects the file at that point of the list. */
    if (hit_rect(mx,my,L.scroll) && file_count) {
        int index = (my - L.scroll.y) * file_count / (L.scroll.h > 0 ? L.scroll.h : 1);
        if (index >= file_count) index = file_count - 1;
        if (index < 0) index = 0;
        redraw_pending=1; selected=index; keep_selection_visible(); return 1;
    }
    for (int k = 0; k < visible_rows && first_visible + k < file_count; ++k) {
        PlayerRect r = row_rect(&L, k);
        if (!r.w) break;
        if (hit_rect(mx,my,r)) { redraw_pending=1; selected=first_visible+k; return 1; }
    }
    return 0;
}
int player_key(int scancode, char character) {
    player_init();
    if (scancode==KEY_SPACE || character==' ') return play_pause();
    if (scancode==KEY_ENTER || character=='\n') return open_selected();
    if (scancode==KEY_UP && selected>0) { redraw_pending=1;--selected;keep_selection_visible();return 1; }
    if (scancode==KEY_DOWN && selected+1<file_count) { redraw_pending=1;++selected;keep_selection_visible();return 1; }
    if (character=='+' || character=='=') { audio_set_volume(audio_status()->volume+5);return 1; }
    if (character=='-') { unsigned volume=audio_status()->volume;audio_set_volume(volume>5?volume-5:0);return 1; }
    if (character=='r' || character=='R') { player_refresh();return 1; }
    if (character=='s' || character=='S') { if (video_mode) video_stop();else audio_stop();return 1; }
    return 0;
}
int player_tick(void) {
    if (!initialized) return 0;
    int frame_changed=video_poll();
    const AudioStatus *status=audio_status();
    int state=video_mode?video_status()->state:status->state;
    int error=video_mode?video_status()->error:status->error;
    unsigned position=(video_mode?video_position_ms():audio_position_ms())/100;
    int audio_progress=!video_mode && position!=last_position;
    int changed=redraw_pending || state!=last_state || error!=last_error || status->volume!=last_volume;
    /* A position-only result must not conceal new decoder/device metadata or
     * a different duration/capacity. played_frames is the only audio field
     * permitted to change without a full client redraw. */
    unsigned capacity=audio_capacity_bytes();
    if (!video_mode && (status->available!=last_audio.available || status->format!=last_audio.format ||
        status->sample_rate!=last_audio.sample_rate || status->channels!=last_audio.channels ||
        status->bits_per_sample!=last_audio.bits_per_sample || status->output_rate!=last_audio.output_rate ||
        status->total_frames!=last_audio.total_frames || status->underruns!=last_audio.underruns ||
        capacity!=last_audio_capacity)) changed=1;
    if (frame_changed && (!video_mode || state!=VIDEO_PLAYING)) changed=1;
    if (error && error!=last_error)
        copy_text(message,video_mode?video_error_string(error):media_error_string(error),sizeof message);
    last_state=state;last_error=error;last_position=position;last_volume=status->volume;
    last_audio=*status;last_audio_capacity=capacity;
    if (timer_ticks()-refresh_time>=TIMER_HZ) {
        player_refresh();
        /* Renames and size changes can keep the same file identity. */
        changed=1;
    }
    redraw_pending=0;
    return changed?PLAYER_CHANGED:audio_progress?PLAYER_AUDIO_PROGRESS:frame_changed?PLAYER_VIDEO_FRAME:0;
}
const char *player_title(void) { return title; }

void player_close(void) { redraw_pending=1;audio_stop();video_stop(); }
