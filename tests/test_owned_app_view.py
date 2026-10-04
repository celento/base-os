"""Production owned-view storage, output and lifecycle with explicit process spies."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from process_host_extract import extract
from test_editor_binding import function

ROOT = Path(__file__).resolve().parents[1]

class OwnedAppViewTests(unittest.TestCase):
    def test_transactional_owned_view_lifetime_and_output(self):
        compiler = shutil.which('clang') or shutil.which('cc')
        with tempfile.TemporaryDirectory(prefix='baseos-owned-view-') as temporary:
            output = Path(temporary) / 'owned-view'
            subprocess.run([compiler, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra',
                            '-Werror', '-fsanitize=address,undefined', '-I', str(ROOT / 'src'),
                            str(ROOT / 'tests/owned_app_view_host.c'), '-o', str(output)], check=True)
            subprocess.run([str(output)], check=True, env=dict(os.environ,
                ASAN_OPTIONS='detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1'))

    def test_real_owned_bex2_process_cleanup_and_publication(self):
        compiler = shutil.which('clang') or shutil.which('cc')
        with tempfile.TemporaryDirectory(prefix='baseos-owned-view-process-') as temporary:
            directory = Path(temporary)
            source = (ROOT / 'src/process.c').read_text()
            extract(source, directory, 'private_space', scheduler=True)
            helpers = ''.join(function(source, name) for name in
                              ('descriptor', 'process_user_root', 'document_parent', 'file_call'))
            (directory / 'private_space_helpers.inc').write_text(helpers)
            image = directory / 'owned-view.bex'
            subprocess.run(['python3', str(ROOT / 'tools/build_app.py'),
                            str(ROOT / 'tests/owned_view_app.c'), str(image), '--format', 'bex2',
                            '--window', 'native-v1', '--workspace-bytes', '0', '--stack-bytes', '16384'], check=True)
            output = directory / 'owned-view-process'
            subprocess.run([compiler, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-Wno-unused-function', '-fsanitize=address,undefined',
                            '-I', str(ROOT / 'src'), '-I', str(directory),
                            str(ROOT / 'tests/owned_app_view_process_host.c'),
                            str(ROOT / 'src/physmem.c'), str(ROOT / 'src/bootinfo.c'),
                            str(ROOT / 'src/executable.c'), '-o', str(output)], check=True)
            for ram in (64, 128):
                subprocess.run([str(output), str(ram), str(image)], check=True, env=dict(os.environ,
                    ASAN_OPTIONS='detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1'))

if __name__ == '__main__':
    unittest.main()
