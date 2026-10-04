"""Ordinary owned native durability coordinator coverage; no QEMU required."""
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class NativeSyncTests(unittest.TestCase):
    def run_host(self, source):
        compiler = shutil.which('clang') or shutil.which('cc')
        self.assertIsNotNone(compiler, 'host C compiler required')
        with tempfile.TemporaryDirectory(prefix='baseos-native-sync-') as temporary:
            binary = pathlib.Path(temporary) / 'native-sync'
            subprocess.run([compiler, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra',
                            '-Werror', '-fsanitize=address,undefined', '-I', str(ROOT / 'src'),
                            str(ROOT / 'tests' / source), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True,
                           env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0',
                                    UBSAN_OPTIONS='halt_on_error=1'))

    def test_owned_completions_and_fs_subscriber(self):
        self.run_host('native_sync_host.c')

    def test_large_profile_owned_completions(self):
        self.run_host('native_sync_large_host.c')


if __name__ == '__main__':
    unittest.main()
