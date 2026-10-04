"""Files selection and input using exact production functions, with real FS.

No guest memory access or hardware emulation; the extracted production functions
use the same valid in-memory volume fixture as the view model host tests.
"""
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

from test_editor_binding import function

ROOT = pathlib.Path(__file__).resolve().parents[1]


class FileViewKernelTests(unittest.TestCase):
    def test_files_selection_and_input(self):
        source = (ROOT / 'src/kernel.c').read_text()
        with tempfile.TemporaryDirectory(prefix='baseos-files-kernel-host-') as temporary:
            directory = pathlib.Path(temporary)
            first = source.index('typedef struct {\n    char buf[EDIT_BUF_SIZE];')
            last = source.index('static WindowState *window_state', first)
            (directory / 'files_kernel_types.inc').write_text(source[first:last])
            first = source.index('#define fm_cwd ')
            last = source.index('static void edit_fingerprint', first)
            operations = source[first:last]
            first = source.index('static const int files_sort_width')
            last = source.index(';', first) + 1
            operations += source[first:last] + '\n'
            for name in ('files_storage_busy', 'fm_set_cwd', 'fm_cwd_valid', 'fm_checked_cwd', 'fm_has_parent',
                         'fm_vis_count', 'fm_row_id', 'fm_refresh', 'fm_rename_cancel',
                         'fm_filter_show', 'fm_filter_clear', 'fm_sort_by', 'fm_filter_key',
                         'fm_select_id', 'fm_new_file', 'fm_rename_begin', 'fm_rename_commit',
                         'do_duplicate', 'files_clipboard_action', 'do_new_folder', 'fm_go_up',
                         'fm_open_selected', 'files_filter_y', 'files_list_y', 'files_rows',
                         'files_scroll', 'input_gesture_ticks', 'double_click', 'handle_files_click', 'files_drop'):
                operations += function(source, name).replace('input_routing ? input_sample_ticks : frame_count', 'frame_count')
            (directory / 'files_kernel_ops.inc').write_text(operations)
            executable = directory / 'files-kernel-host'
            compiler = shutil.which('clang') or shutil.which('cc')
            subprocess.run([compiler, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-Wno-unused-function', '-fsanitize=address,undefined',
                            '-I', str(ROOT / 'src'), '-I', str(directory),
                            str(ROOT / 'tests/file_view_kernel_host.c'),
                            str(ROOT / 'src/file_view.c'), str(ROOT / 'src/file_clipboard.c'),
                            '-o', str(executable)], check=True)
            environment = dict(os.environ, ASAN_OPTIONS=os.environ.get('ASAN_OPTIONS', 'detect_leaks=0'),
                               UBSAN_OPTIONS='halt_on_error=1')
            subprocess.run([str(executable)], check=True, env=environment)


if __name__ == '__main__':
    unittest.main()
