"""Normal desktop native dispatch and example durability feedback, ASan/UBSan."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_editor_binding import function
from desktop_source import block, chrome_defines, defines

ROOT = Path(__file__).resolve().parents[1]


class NativeLaunchTests(unittest.TestCase):
    def compile_run(self, name, directory, extra=()):
        compiler = shutil.which('clang') or shutil.which('cc')
        output = directory / name
        subprocess.run([compiler, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-function', '-fsanitize=address,undefined',
                        '-I', str(ROOT / 'src'), '-I', str(ROOT / 'sdk'), '-I', str(directory),
                        str(ROOT / 'tests' / (name + '.c')), *map(str, extra),
                        '-o', str(output)], check=True)
        subprocess.run([str(output)], check=True,
                       env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1'))

    def test_desktop_file_dispatch_and_owned_start(self):
        source = (ROOT / 'src/shell/desktop/desktop.c').read_text()
        with tempfile.TemporaryDirectory(prefix='baseos-native-launch-host-') as tmp:
            directory = Path(tmp)
            start = source.index('typedef struct {\n    char name[FS_NAME_LEN]; /* A result owns')
            end = source.index('static LaunchItem launch_items', start)
            launch_errors = source[source.index('enum { NATIVE_LAUNCH_WINDOW='):
                                   source.index('static const char *native_launch_error')]
            # Work-area geometry of the installed style for window placement,
            # and the Open/Save As browser state with its Save As types.
            geometry = chrome_defines(source) + defines(source, ('TASKBAR_H', 'TASKBAR_Y'))
            picker = block(source, 'static FileViewOptions pick_view;', 'static int name_target_valid(void)',
                           include_end=False)
            (directory / 'native_launch_types.inc').write_text(geometry + source[start:end] + launch_errors + picker)
            placement = block(source, '/* New windows cascade from the top left', '\n}\n')
            names = ('name_target_valid', 'win_close', 'fm_vis_count', 'fm_row_id', 'fm_refresh', 'file_extension',
                     'native_launch_error', 'native_window_start', 'native_file_mode',
                     'open_native_file', 'open_fs_file', 'fm_open_selected',
                     'od_row_enabled', 'od_next_enabled', 'od_refresh', 'pick_go', 'od_open_selected',
                     'str_has', 'name_before', 'launcher_dos_app', 'launch_label', 'launcher_refresh',
                     'launcher_close', 'launcher_run')
            (directory / 'native_launch_ops.inc').write_text(placement + ''.join(function(source, name) for name in names))
            self.compile_run('native_launch_host', directory,
                             [ROOT / 'tests/net_stub.c', ROOT / 'src/kernel/download.c', ROOT / 'src/apps/file_view.c', '-DDOWNLOAD_HOST_TEST'])

    def test_example_save_feedback(self):
        with tempfile.TemporaryDirectory(prefix='baseos-native-save-host-') as tmp:
            self.compile_run('native_save_host', Path(tmp))

    def test_owned_window_factory_transaction(self):
        source = (ROOT / 'src/shell/desktop/desktop.c').read_text()
        process = (ROOT / 'src/kernel/process.c').read_text()
        with tempfile.TemporaryDirectory(prefix='baseos-window-factory-') as tmp:
            directory = Path(tmp)
            declarations = source[source.index('enum { NATIVE_LAUNCH_WINDOW='):
                                  source.index('static const char *native_launch_error')]
            names = ('native_launch_error', 'native_window_start', 'native_file_mode',
                     'terminal_native_launch', 'open_native_file')
            code = chrome_defines(source) + defines(source, ('TASKBAR_H', 'TASKBAR_Y')) + declarations
            code += block(source, '/* New windows cascade from the top left', '\n}\n')
            code += ''.join(function(source, name) for name in names)
            (directory / 'native_window_factory.inc').write_text(code)
            process = process.replace('int process_probe_launch(', 'static int process_probe_launch(')
            code = ''.join(function(process, name) for name in
                           ('valid_image', 'image_magic', 'image_plan', 'process_probe_launch'))
            code = code.replace('static int process_probe_launch(', 'int process_probe_launch(')
            (directory / 'native_window_probe.inc').write_text(code)
            self.compile_run('native_window_factory_host', directory, [ROOT / 'src/kernel/executable.c'])


if __name__ == '__main__':
    unittest.main()
