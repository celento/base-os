/*
 * BaseOS Todo: a capped list of items plus the new-item field.
 * Model only - kernel.c owns the pixels. Backed by /prefs/todo.
 */
#include "fs.h"
#include "todo.h"

#define TODO_FILE_MAX (TODO_MAX * (FS_NAME_LEN + 2) + 1)
typedef struct { char text[FS_NAME_LEN]; int done; } TodoItem;
static TodoItem td[TODO_MAX];
static int td_n;
static char td_field_buf[FS_NAME_LEN];
static int td_field_len, td_field_on;

/* Only QUEUED is automatically retried. Other failures keep the newest list
 * private until another accepted edit, explicit Retry, or shutdown preparation.
 * The small source copy detects replacement/editing through Files or Editor;
 * saved node numbers alone are not a binding. No borrowed FS pointers survive. */
enum {
    TODO_IDLE, TODO_QUEUED, TODO_RAM, TODO_SAVED, TODO_PATH_FAILED,
    TODO_FULL_FAILED, TODO_WRITE_FAILED, TODO_CONFLICT, TODO_SYNC_FAILED, TODO_RELOAD,
    TODO_UNSUPPORTED
};
static int td_pending, td_state;
static int td_source_file = -1, td_source_len;
static unsigned td_source_identity;
static char td_source[TODO_FILE_MAX];

static int todo_file(void) {
    int p = fs_find_child(fs_root(), "prefs");
    return fs_is_dir(p) ? fs_find_child(p, "todo") : -1;
}
static int todo_source_matches(int f) {
    if (f < 0 || f != td_source_file || fs_identity(f) != td_source_identity ||
        fs_is_dir(f) || fs_is_app(f) || fs_size(f) != td_source_len)
        return 0;
    const char *data = fs_data(f);
    for (int i = 0; i < td_source_len; ++i)
        if (data[i] != td_source[i]) return 0;
    return 1;
}
static void todo_capture_source(int f, const char *data, int n) {
    td_source_file = f;
    td_source_identity = fs_identity(f);
    td_source_len = n;
    kmemcpy(td_source, data, n);
}
static void todo_observe(void) {
    if (td_pending || (td_state != TODO_RAM && td_state != TODO_SYNC_FAILED &&
                       td_state != TODO_SAVED)) return;
    if (!todo_source_matches(todo_file())) {
        /* A later external edit cannot make our already durable list dirty.
         * Reopening may load it. Only unsaved accepted work needs protecting. */
        td_pending = td_state != TODO_SAVED;
        td_state = td_pending ? TODO_CONFLICT : TODO_RELOAD;
    } else if (td_state != TODO_SAVED) {
        if (fs_storage_status()) td_state = TODO_SYNC_FAILED;
        else if (!fs_needs_sync()) td_state = TODO_SAVED;
        else td_state = TODO_RAM;
    }
}
static void todo_save(void) {
    /* Busy must be checked before even creating /prefs. All private list edits
     * coalesce during the lease, including edits made while Todo is closed. */
    if (fs_sync_busy()) { td_state = TODO_QUEUED; return; }
    int p = fs_find_child(fs_root(), "prefs");
    if (p >= 0 && (!fs_is_dir(p) || fs_is_app(p))) {
        td_state = TODO_PATH_FAILED;
        return;
    }
    int f = p < 0 ? -1 : fs_find_child(p, "todo");
    if (f >= 0 && (fs_is_dir(f) || fs_is_app(f))) {
        td_state = TODO_PATH_FAILED;
        return;
    }
    if (f >= 0 && !todo_source_matches(f)) {
        td_state = TODO_CONFLICT;
        return;
    }
    char buf[TODO_FILE_MAX];
    int n = 0;
    for (int i = 0; i < td_n; i++) {
        buf[n++] = td[i].done ? 'x' : ' ';
        for (int j = 0; td[i].text[j]; j++) buf[n++] = td[i].text[j];
        buf[n++] = '\n';
    }
    /* Account for both nodes and their reduced IDE payload allowance before
     * creating anything. Replacement may reclaim the old file's bytes. */
    unsigned count = (unsigned)fs_node_count() + (p < 0) + (f < 0);
    unsigned capacity = fs_capacity_for_nodes(count);
    unsigned used = fs_used_bytes() - (unsigned)(f < 0 ? 0 : fs_size(f));
    if (count > (unsigned)fs_node_limit() || used > capacity ||
        (unsigned)n > capacity - used || (unsigned)n > fs_file_limit()) {
        td_state = TODO_FULL_FAILED;
        return;
    }
    int new_dir = p < 0, new_file = f < 0, result;
    if (new_dir) p = fs_mkdir(fs_root(), "prefs");
    if (p < 0) { td_state = p == FS_ERR_BUSY ? TODO_QUEUED : TODO_WRITE_FAILED; return; }
    if (new_file) f = fs_create(p, "todo");
    result = f < 0 ? f : fs_write(f, buf, n);
    if (result != n) {
        /* Ordinary filesystem calls are cooperative: no app or autosave starts
         * inside this operation. Roll back only nodes allocated by this call. */
        if (new_file && f >= 0) (void)fs_delete(f);
        if (new_dir) (void)fs_delete(p);
        td_state = result == FS_ERR_BUSY ? TODO_QUEUED : TODO_WRITE_FAILED;
        return;
    }
    todo_capture_source(f, buf, n);
    td_pending = 0;
    td_state = TODO_RAM;
    todo_observe();
}
int todo_tick(void) {
    int previous = td_state;
    if (!fs_sync_busy()) {
        todo_observe();
        if (td_pending && td_state == TODO_QUEUED) todo_save();
    }
    return previous != td_state;
}
const char *todo_status(void) {
    switch (td_state) {
    case TODO_QUEUED: return "Tasks queued; disk is saving.";
    case TODO_RAM: return "Tasks in RAM; waiting for disk save.";
    case TODO_SAVED: return "Tasks saved.";
    case TODO_PATH_FAILED: return "Todo path occupied; fix it, then Ctrl+S.";
    case TODO_FULL_FAILED: return "No room; free space, then Ctrl+S.";
    case TODO_WRITE_FAILED: return "Tasks not saved; Ctrl+S to retry.";
    case TODO_CONFLICT: return "Todo file changed; fix path, then Ctrl+S.";
    case TODO_SYNC_FAILED: return "RAM only; disk save failed. Ctrl+S retries.";
    case TODO_RELOAD: return "Todo file changed; reopen to reload.";
    case TODO_UNSUPPORTED: return "Unsupported Todo file; existing bytes kept.";
    default: return "";
    }
}
int todo_retry_save(void) {
    if (fs_sync_busy()) {
        if (td_pending) td_state = TODO_QUEUED;
        return FS_ERR_BUSY;
    }
    todo_observe();
    if (td_pending) todo_save();
    if (td_pending) return -1;
    int result = fs_sync();
    todo_tick();
    return result;
}
int todo_prepare_shutdown(void) {
    /* Join only to release the lease. Even a failed older snapshot must not
     * prevent putting the latest accepted list in RAM. The caller's final
     * fs_sync, after all shutdown staging, is the required durability check. */
    if (fs_sync_busy()) (void)fs_sync();
    if (fs_sync_busy()) return FS_ERR_BUSY;
    todo_observe();
    if (td_pending) todo_save();
    return td_pending ? -1 : 0;
}
static void todo_changed(void) {
    td_pending = 1;
    td_state = TODO_QUEUED;
    todo_tick();
}
static int todo_source_supported(const char *data, int size) {
    int pos=0, rows=0;
    while(pos<size){
        if(data[pos]=='\n'){pos++;continue;}
        char mark=data[pos++];
        if(mark!=' '&&mark!='x'&&mark!='X')return 0;
        int length=0;
        while(pos<size&&data[pos]!='\n'){
            unsigned char c=(unsigned char)data[pos++];
            if(c<' '||c>'~'||++length>=FS_NAME_LEN)return 0;
        }
        if(length&&++rows>TODO_MAX)return 0;
        if(pos<size)pos++;
    }
    return 1;
}
void todo_load(void) {
    /* Closing/reopening a window must not discard deferred edits or its field.
     * Reload from disk only when no accepted/private work needs preserving. */
    if (td_pending || td_field_len || td_state == TODO_RAM || td_state == TODO_SYNC_FAILED)
        return;
    td_state = TODO_IDLE;
    td_source_file = -1;
    td_source_identity = 0;
    td_source_len = 0;
    td_n = 0;
    td_field_len = 0;
    td_field_buf[0] = 0;
    td_field_on = 0;
    int p = fs_find_child(fs_root(), "prefs");
    if (p < 0) return;
    if (!fs_is_dir(p) || fs_is_app(p)) { td_state = TODO_PATH_FAILED; return; }
    int f = fs_find_child(p, "todo");
    if (f < 0) return;
    if (fs_is_dir(f) || fs_is_app(f)) { td_state = TODO_PATH_FAILED; return; }
    char buf[TODO_FILE_MAX];
    int n = fs_read(f, buf, (int)sizeof buf);
    if (n < 0) { td_state = TODO_WRITE_FAILED; return; }
    /* Never bind a truncated or unrelated text file as an editable Todo list. */
    if(fs_size(f)!=n||!todo_source_supported(buf,n)){
        td_state=TODO_UNSUPPORTED;
        return;
    }
    todo_capture_source(f, buf, n);
    td_state = TODO_RAM;
    if (!fs_sync_busy()) todo_observe();
    int i = 0;
    while (i < n && td_n < TODO_MAX) {
        if (buf[i] == '\n') { i++; continue; }
        int done = buf[i] == 'x' || buf[i] == 'X';
        i++;
        int len = 0;
        while (i < n && buf[i] != '\n') {
            if (len < FS_NAME_LEN - 1) td[td_n].text[len++] = buf[i];
            i++;
        }
        td[td_n].text[len] = 0;
        if (i < n) i++;
        if (len > 0) { td[td_n].done = done; td_n++; }
    }
}
int todo_count(void) { return td_n; }
int todo_done(int i) { return i >= 0 && i < td_n ? td[i].done : 0; }
const char *todo_text(int i) { return i >= 0 && i < td_n ? td[i].text : ""; }
int todo_toggle(int i) {
    if (i < 0 || i >= td_n) return 0;
    td[i].done = !td[i].done;
    todo_changed();
    return 1;
}
void todo_focus_field(void) { td_field_on = 1; }
int todo_field_active(void) { return td_field_on; }
const char *todo_field(void) { return td_field_buf; }
int todo_field_char(char c) {
    if (!td_field_on || c < ' ' || c > '~') return 0;
    if (td_field_len >= FS_NAME_LEN - 1) return 0;
    td_field_buf[td_field_len++] = c;
    td_field_buf[td_field_len] = 0;
    return 1;
}
int todo_field_backspace(void) {
    if (!td_field_on || td_field_len == 0) return 0;
    td_field_buf[--td_field_len] = 0;
    return 1;
}
int todo_field_enter(void) {
    if (!td_field_on || td_field_len == 0 || td_n >= TODO_MAX) return 0;
    kstrcpy(td[td_n].text, td_field_buf);
    td[td_n].done = 0;
    td_n++;
    td_field_len = 0;
    td_field_buf[0] = 0;
    todo_changed();
    return 1;
}
