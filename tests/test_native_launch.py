"""Normal desktop native dispatch and example durability feedback, ASan/UBSan."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_editor_binding import function

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
        source = (ROOT / 'src/kernel.c').read_text()
        with tempfile.TemporaryDirectory(prefix='baseos-native-launch-host-') as tmp:
            directory = Path(tmp)
            start = source.index('typedef struct {\n    char name[FS_NAME_LEN]; /* A result owns')
            end = source.index('static LaunchItem launch_items', start)
            launch_errors = source[source.index('enum { NATIVE_LAUNCH_WINDOW='):
                                   source.index('static const char *native_launch_error')]
            (directory / 'native_launch_types.inc').write_text(source[start:end] + launch_errors)
            names = ('win_close', 'fm_vis_count', 'fm_row_id', 'fm_refresh', 'file_extension',
                     'native_launch_error', 'native_window_start', 'native_file_mode',
                     'open_native_file', 'open_fs_file', 'fm_open_selected',
                     'od_row_enabled', 'od_next_enabled', 'od_refresh', 'od_open_selected',
                     'str_has', 'launcher_refresh', 'launcher_close', 'launcher_run')
            (directory / 'native_launch_ops.inc').write_text(''.join(function(source, name) for name in names))
            self.compile_run('native_launch_host', directory,
                             [ROOT / 'tests/net_stub.c', ROOT / 'src/download.c', ROOT / 'src/file_view.c', '-DDOWNLOAD_HOST_TEST'])

    def test_example_save_feedback(self):
        with tempfile.TemporaryDirectory(prefix='baseos-native-save-host-') as tmp:
            self.compile_run('native_save_host', Path(tmp))

    def test_owned_window_factory_transaction(self):
        source = (ROOT / 'src/kernel.c').read_text()
        process = (ROOT / 'src/process.c').read_text()
        with tempfile.TemporaryDirectory(prefix='baseos-window-factory-') as tmp:
            directory = Path(tmp)
            declarations = source[source.index('enum { NATIVE_LAUNCH_WINDOW='):
                                  source.index('static const char *native_launch_error')]
            names = ('native_launch_error', 'native_window_start', 'native_file_mode',
                     'terminal_native_launch', 'open_native_file')
            code = declarations + ''.join(function(source, name) for name in names)
            (directory / 'native_window_factory.inc').write_text(code)
            process = process.replace('int process_probe_launch(', 'static int process_probe_launch(')
            code = ''.join(function(process, name) for name in
                           ('valid_image', 'image_magic', 'image_plan', 'process_probe_launch'))
            code = code.replace('static int process_probe_launch(', 'int process_probe_launch(')
            (directory / 'native_window_probe.inc').write_text(code)
            self.compile_run('native_window_factory_host', directory, [ROOT / 'src/executable.c'])


if __name__ == '__main__':
    unittest.main()
