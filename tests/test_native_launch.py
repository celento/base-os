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
            (directory / 'native_launch_types.inc').write_text(source[start:end])
            names = ('fm_vis_count', 'fm_row_id', 'fm_refresh', 'file_extension',
                     'open_native_file', 'open_fs_file', 'fm_open_selected',
                     'od_row_enabled', 'od_next_enabled', 'od_refresh', 'od_open_selected',
                     'str_has', 'launcher_refresh', 'launcher_close', 'launcher_run')
            (directory / 'native_launch_ops.inc').write_text(''.join(function(source, name) for name in names))
            self.compile_run('native_launch_host', directory,
                             [ROOT / 'tests/net_stub.c', ROOT / 'src/download.c', '-DDOWNLOAD_HOST_TEST'])

    def test_example_save_feedback(self):
        with tempfile.TemporaryDirectory(prefix='baseos-native-save-host-') as tmp:
            self.compile_run('native_save_host', Path(tmp))


if __name__ == '__main__':
    unittest.main()
