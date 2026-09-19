/*
 * BaseOS Todo: a capped list of items plus the new-item field.
 * Model only - kernel.c owns the pixels. Backed by /prefs/todo.
 */

#include "fs.h"
#include "todo.h"

#define TODO_FILE_MAX (TODO_MAX * (FS_NAME_LEN + 2) + 1)

typedef struct {
    char text[FS_NAME_LEN];
    int done;
} TodoItem;

static TodoItem td[TODO_MAX];
static int td_n;

static char td_field_buf[FS_NAME_LEN];
static int td_field_len;
static int td_field_on;

static int prefs_dir(void) {
    int p = fs_find_child(fs_root(), "prefs");
    if (p < 0)
        p = fs_mkdir(fs_root(), "prefs");
    return p;
}

static void todo_save(void) {
    int p = prefs_dir();
    if (p < 0)
        return;
    int f = fs_find_child(p, "todo");
    if (f < 0)
        f = fs_create(p, "todo");
    if (f < 0)
        return;
    char buf[TODO_FILE_MAX];
    int n = 0;
    for (int i = 0; i < td_n; i++) {
        buf[n++] = td[i].done ? 'x' : ' ';
        for (int j = 0; td[i].text[j]; j++)
            buf[n++] = td[i].text[j];
        buf[n++] = '\n';
    }
    fs_write(f, buf, n);
}

void todo_load(void) {
    td_n = 0;
    td_field_len = 0;
    td_field_buf[0] = 0;
    td_field_on = 0;

    int p = fs_find_child(fs_root(), "prefs");
    if (p < 0)
        return;
    int f = fs_find_child(p, "todo");
    if (f < 0)
        return;

    char buf[TODO_FILE_MAX];
    int n = fs_read(f, buf, (int)sizeof buf);
    if (n <= 0)
        return;

    int i = 0;
    while (i < n && td_n < TODO_MAX) {
        if (buf[i] == '\n') {
            i++;
            continue;
        }
        int done = buf[i] == 'x' || buf[i] == 'X';
        i++;
        int len = 0;
        while (i < n && buf[i] != '\n') {
            if (len < FS_NAME_LEN - 1)
                td[td_n].text[len++] = buf[i];
            i++;
        }
        td[td_n].text[len] = 0;
        if (i < n)
            i++;
        if (len > 0) {
            td[td_n].done = done;
            td_n++;
        }
    }
}

int todo_count(void) { return td_n; }

int todo_done(int i) {
    return i >= 0 && i < td_n ? td[i].done : 0;
}

const char *todo_text(int i) {
    return i >= 0 && i < td_n ? td[i].text : "";
}

int todo_toggle(int i) {
    if (i < 0 || i >= td_n)
        return 0;
    td[i].done = !td[i].done;
    todo_save();
    return 1;
}

void todo_focus_field(void) { td_field_on = 1; }

int todo_field_active(void) { return td_field_on; }

const char *todo_field(void) { return td_field_buf; }

int todo_field_char(char c) {
    if (!td_field_on || c < ' ' || c > '~')
        return 0;
    if (td_field_len >= FS_NAME_LEN - 1)
        return 0;
    td_field_buf[td_field_len++] = c;
    td_field_buf[td_field_len] = 0;
    return 1;
}

int todo_field_backspace(void) {
    if (!td_field_on || td_field_len == 0)
        return 0;
    td_field_buf[--td_field_len] = 0;
    return 1;
}

int todo_field_enter(void) {
    if (!td_field_on || td_field_len == 0 || td_n >= TODO_MAX)
        return 0;
    kstrcpy(td[td_n].text, td_field_buf);
    td[td_n].done = 0;
    td_n++;
    td_field_len = 0;
    td_field_buf[0] = 0;
    todo_save();
    return 1;
}
