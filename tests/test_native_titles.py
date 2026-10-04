"""Production desktop title selection uses copied per-task metadata."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_editor_binding import function

ROOT = Path(__file__).resolve().parents[1]


class NativeTitleTests(unittest.TestCase):
    def test_title_selection_and_lifetime(self):
        implementation = function((ROOT / 'src/kernel.c').read_text(), 'win_display_title')
        with tempfile.TemporaryDirectory(prefix='baseos-native-title-') as temporary:
            source = Path(temporary) / 'test.c'
            source.write_text('''#include <assert.h>
#include <string.h>
#include "term.h"
#define MAX_WIN 8
#define WK_TERM 11
static struct { int kind; } wins[MAX_WIN];
static int live[MAX_WIN], calls;
static const char *names[MAX_WIN];
static const char *win_app_name(int kind) { return kind == WK_TERM ? "Terminal" : "Files"; }
int term_task_title(int slot, char *out, int capacity) {
    assert(slot >= 0 && slot < MAX_WIN && capacity == TERM_TASK_TITLE_LEN);
    ++calls; out[0] = 0;
    if (!live[slot]) return 0;
    strcpy(out, names[slot]); return 1;
}
''' + implementation + '''
int main(void) {
    char one[TERM_TASK_TITLE_LEN], two[TERM_TASK_TITLE_LEN];
    assert(!strcmp(win_display_title(-1, one), "App"));
    assert(!strcmp(win_display_title(MAX_WIN, one), "App"));
    assert(!calls && !strcmp(win_display_title(0, one), "Files"));
    wins[2].kind = wins[6].kind = WK_TERM;
    assert(!strcmp(win_display_title(2, one), "Terminal"));
    live[2] = live[6] = 1;
    names[2] = "docstats.bex - project.txt";
    names[6] = "counter.bex";
    assert(win_display_title(2, one) == one);
    assert(win_display_title(6, two) == two);
    assert(!strcmp(one, "docstats.bex - project.txt"));
    assert(!strcmp(two, "counter.bex"));
    live[2] = 0;
    assert(!strcmp(win_display_title(2, one), "Terminal"));
    assert(!strcmp(two, "counter.bex"));
    return 0;
}
''')
            compiler = shutil.which('clang') or shutil.which('cc')
            output = Path(temporary) / 'test'
            subprocess.run([compiler, '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-I', str(ROOT / 'src'),
                            str(source), '-o', str(output)], check=True)
            subprocess.run([str(output)], check=True,
                           env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1'))
