#include "player.h"
#include "audio.h"
#include "app.h"
#include "fs.h"
#include "platform.h"

#define PLAYER_ROW 28
static int files[FS_MAX_NODES], file_count, selected, first_visible, visible_rows = 4;
static unsigned identities[FS_MAX_NODES];
static int source_id = -1;
static unsigned source_identity;
static char title[FS_NAME_LEN], message[96];
static unsigned refresh_time, last_position, last_volume;
static int last_state, last_error, initialized;

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
static int is_audio(int id) {
    const char *name = fs_name(id);
    return extension(name, ".wav") || extension(name, ".wave") || extension(name, ".mp3");
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
    int old_id = selected >= 0 && selected < file_count ? files[selected] : -1;
    unsigned old_identity = selected >= 0 && selected < file_count ? identities[selected] : 0;
    file_count = 0; selected = -1;
    int trash = fs_find_child(fs_root(), "trash");
    for (int id = 0; id < FS_MAX_NODES; ++id) {
        if (!fs_valid(id) || fs_is_dir(id) || fs_is_app(id) || !is_audio(id)) continue;
        int parent = fs_parent(id), excluded = 0;
        for (int depth = 0; depth < FS_MAX_NODES && parent > 0; ++depth) {
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
    if (!fs_valid(id) || fs_is_dir(id) || fs_is_app(id)) {
        copy_text(message, "This audio file is no longer available.", sizeof message);
        return MEDIA_BAD_FILE;
    }
    int bytes = fs_size(id);
    if (bytes <= 0 || (unsigned)bytes > audio_capacity_bytes()) {
        int error = bytes <= 0 ? MEDIA_BAD_FILE : MEDIA_TOO_LARGE;
        copy_text(message, media_error_string(error), sizeof message); return error;
    }
    int error = audio_play(fs_data(id), (unsigned)bytes);
    if (error) { copy_text(message, media_error_string(error), sizeof message); return error; }
    source_id = id; source_identity = fs_identity(id);
    copy_text(title, fs_name(id), sizeof title); message[0] = 0;
    player_refresh();
    for (int i = 0; i < file_count; ++i) if (files[i] == id) selected = i;
    keep_selection_visible(); return MEDIA_OK;
}
static int open_selected(void) {
    if (selected < 0 || selected >= file_count) {
        copy_text(message, "Add a WAV or MP3 file, then choose Play.", sizeof message); return 1;
    }
    if (!fs_valid(files[selected]) || fs_identity(files[selected]) != identities[selected]) {
        player_refresh(); copy_text(message, "The selected file changed. Choose it again.", sizeof message); return 1;
    }
    player_open_file(files[selected]); return 1;
}
static int play_pause(void) {
    const AudioStatus *status = audio_status();
    if (status->state == AUDIO_PLAYING) { audio_pause(1); return 1; }
    if (status->state == AUDIO_PAUSED) { audio_pause(0); return 1; }
    if (status->state == AUDIO_LOADING) return 0;
    if (selected >= 0 && selected < file_count) return open_selected();
    if (source_id >= 0 && fs_valid(source_id) && fs_identity(source_id) == source_identity) {
        player_open_file(source_id); return 1;
    }
    copy_text(message, "Choose an audio file from the list below.", sizeof message); return 1;
}
static void time_string(char *out, unsigned milliseconds) {
    unsigned seconds = milliseconds / 1000;
    fmt_uint(out, seconds / 60);
    unsigned n = (unsigned)kstrlen(out); out[n++] = ':';
    fmt_pad2(out+n, (int)(seconds % 60));
    if (audio_duration_ms() < 10000) {
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
static void button(int x, int y, int w, const char *label, int primary) {
    draw_round_rect(x, y, w, 32, 6, primary ? app_accent : app_chrome);
    if (!primary) draw_round_frame(x, y, w, 32, 6, app_chrome_dk);
    int text_x = x + (w - ui_string_w(label)) / 2;
    draw_string(label, text_x, y + 7, primary ? COLOR_WHITE : app_text);
}
void player_draw(int x, int y, int w, int h) {
    player_init();
    draw_rect(x, y, w, h, COLOR_WHITE);
    if (w < 420 || h < 362) {
        draw_string_clip("Enlarge Media Player to show its controls.", x+8, y+12, app_text, x+w-8); return;
    }
    const AudioStatus *status = audio_status();
    draw_string_bold("Media Player", x+16, y+12, app_text);
    const char *device = status->available ? "Sound Blaster 16" : "No audio device";
    draw_string(device, x+w-16-ui_string_w(device), y+12, app_text_dim);
    draw_round_rect(x+16, y+42, w-32, 116, 8, app_chrome);
    draw_string_bold_clip(title[0] ? title : "Choose a track", x+30, y+56, app_text, x+w-128);
    const char *state = state_string(status->state);
    draw_string(state, x+w-30-ui_string_w(state), y+56, status->state==AUDIO_PLAYING ? app_accent_dk : app_text_dim);
    char text[96] = {0}, number[20];
    if (title[0]) {
        copy_text(text, status->format==AUDIO_FORMAT_MP3 ? "MP3" : "PCM WAV", sizeof text);
        append(text, status->channels==2 ? " / Stereo / " : " / Mono / ", sizeof text);
        fmt_uint(number, status->sample_rate); append(text, number, sizeof text); append(text, " Hz", sizeof text);
    } else copy_text(text, "Your music, played directly by BaseOS.", sizeof text);
    draw_string_clip(text, x+30, y+84, app_text_dim, x+w-30);
    int bar_width = w-60;
    draw_round_rect(x+30, y+114, bar_width, 6, 3, app_chrome_dk);
    unsigned position = audio_position_ms(), duration = audio_duration_ms();
    unsigned scale = duration / 65535u + 1;
    unsigned progress = duration ? (position/scale)*(unsigned)bar_width/(duration/scale) : 0;
    if (progress > (unsigned)bar_width) progress = (unsigned)bar_width;
    if (progress) draw_rect(x+30, y+114, (int)progress, 6, app_accent);
    time_string(text, position); draw_string(text, x+30, y+130, app_text_dim);
    time_string(text, duration); draw_string(text, x+w-30-ui_string_w(text), y+130, app_text_dim);
    const char *play = status->state==AUDIO_PLAYING ? "Pause" : status->state==AUDIO_LOADING ? "Loading" : "Play";
    button(x+16, y+170, 96, play, 1);
    button(x+120, y+170, 76, "Stop", 0);
    button(x+204, y+170, 96, "Clear error", 0);
    button(x+w-96, y+170, 80, "Refresh", 0);
    draw_string("Volume", x+16, y+216, app_text_dim);
    int volume_width = w-176;
    draw_round_rect(x+96, y+222, volume_width, 6, 3, app_chrome_dk);
    int volume_pixels = (int)(status->volume*(unsigned)volume_width/100);
    if (volume_pixels) draw_rect(x+96, y+222, volume_pixels, 6, app_accent);
    draw_round_rect(x+91+volume_pixels, y+217, 10, 16, 4, app_accent_dk);
    fmt_uint(text, status->volume); append(text, "%", sizeof text);
    draw_string(text, x+w-64, y+216, app_text);
    const char *hint = message[0] ? message : "Space: play/pause   Enter: open   +/-: volume";
    draw_string_clip(hint, x+16, y+244, message[0] ? gfx_rgb(176,47,42) : app_text_dim, x+w-16);
    copy_text(text, "Audio files (", sizeof text); fmt_uint(number, (unsigned)file_count);
    append(text, number, sizeof text); append(text, ")", sizeof text);
    draw_string_bold(text, x+16, y+272, app_text);
    draw_string("^", x+w-66, y+272, app_text_dim); draw_string("v", x+w-36, y+272, app_text_dim);
    int list_y = y+298, list_h = h-334;
    visible_rows = list_h/PLAYER_ROW;
    if (visible_rows < 1) visible_rows = 1;
    keep_selection_visible();
    draw_round_frame(x+16, list_y, w-32, list_h, 6, app_chrome_dk);
    if (!file_count) {
        draw_string_clip("No audio files. Import a .wav or .mp3 to begin.", x+26, list_y+10, app_text_dim, x+w-26);
    } else for (int row = 0; row < visible_rows && first_visible+row < file_count; ++row) {
        int index = first_visible+row, id = files[index], row_y = list_y+row*PLAYER_ROW;
        if (index == selected) draw_rect(x+18, row_y+1, w-44, PLAYER_ROW-1, gfx_lighter(app_accent, 85));
        int current = id == source_id && fs_identity(id) == source_identity;
        draw_round_rect(x+27, row_y+10, 6, 6, 3, current ? app_accent : app_chrome_dk);
        draw_string_clip(fs_name(id), x+43, row_y+5, app_text, x+w-100);
        const char *format = extension(fs_name(id), ".mp3") ? "MP3" : "WAV";
        draw_string(format, x+w-80, row_y+5, app_text_dim);
    }
    if (file_count > visible_rows) {
        int track = list_h-4, thumb = track*visible_rows/file_count;
        if (thumb < 8) thumb = 8;
        int offset = (track-thumb)*first_visible/(file_count-visible_rows);
        draw_round_rect(x+w-24, list_y+2+offset, 4, thumb, 2, app_chrome_dk);
    }
    copy_text(text, "WAV + MP3 / ", sizeof text); fmt_uint(number, audio_capacity_bytes()/1024);
    append(text, number, sizeof text); append(text, " KiB playback limit", sizeof text);
    draw_string_clip(text, x+16, y+h-24, app_text_dim, x+w-16);
}
int player_click(int x, int y, int w, int h, int mx, int my) {
    player_init();
    if (w < 420 || h < 362) return 0;
    if (hit(mx,my,x+16,y+170,96,32)) return play_pause();
    if (hit(mx,my,x+120,y+170,76,32)) { audio_stop(); return 1; }
    if (hit(mx,my,x+204,y+170,96,32)) { message[0]=0; audio_clear_error(); return 1; }
    if (hit(mx,my,x+w-96,y+170,80,32)) { player_refresh(); return 1; }
    if (hit(mx,my,x+91,y+210,w-166,30)) {
        int volume = (mx-(x+96))*100/(w-176);
        audio_set_volume((unsigned)(volume<0 ? 0 : volume>100 ? 100 : volume)); return 1;
    }
    if (hit(mx,my,x+w-76,y+268,28,24)) return player_key(KEY_UP,0);
    if (hit(mx,my,x+w-46,y+268,28,24)) return player_key(KEY_DOWN,0);
    if (hit(mx,my,x+16,y+298,w-32,h-334)) {
        int row = (my-(y+298))/PLAYER_ROW;
        if (row<visible_rows && first_visible+row<file_count) {
            selected=first_visible+row; return 1;
        }
    }
    return 0;
}
int player_key(int scancode, char character) {
    player_init();
    if (scancode==KEY_SPACE || character==' ') return play_pause();
    if (scancode==KEY_ENTER || character=='\n') return open_selected();
    if (scancode==KEY_UP && selected>0) { --selected;keep_selection_visible();return 1; }
    if (scancode==KEY_DOWN && selected+1<file_count) { ++selected;keep_selection_visible();return 1; }
    if (character=='+' || character=='=') { audio_set_volume(audio_status()->volume+5);return 1; }
    if (character=='-') { unsigned volume=audio_status()->volume;audio_set_volume(volume>5?volume-5:0);return 1; }
    if (character=='r' || character=='R') { player_refresh();return 1; }
    if (character=='s' || character=='S') { audio_stop();return 1; }
    return 0;
}
int player_tick(void) {
    if (!initialized) return 0;
    const AudioStatus *status=audio_status();
    unsigned position=audio_position_ms()/100;
    int changed=status->state!=last_state || status->error!=last_error || position!=last_position || status->volume!=last_volume;
    if (status->error && status->error!=last_error)
        copy_text(message,media_error_string(status->error),sizeof message);
    last_state=status->state;last_error=status->error;last_position=position;last_volume=status->volume;
    if (timer_ticks()-refresh_time>=TIMER_HZ) {
        player_refresh();
        /* Renames and size changes can keep the same file identity. */
        changed=1;
    }
    return changed;
}
const char *player_title(void) { return title; }
