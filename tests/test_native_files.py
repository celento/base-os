"""Version-bound native documents through real FS, normal lifecycle and limits."""
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class NativeFileTests(unittest.TestCase):
    def test_coherence_lifetimes_capacities_and_finite_generations(self):
        compiler = shutil.which('clang') or shutil.which('cc')
        self.assertIsNotNone(compiler, 'host C compiler required')
        with tempfile.TemporaryDirectory(prefix='baseos-native-files-') as directory:
            binary = pathlib.Path(directory) / 'native-files'
            subprocess.run([compiler, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra',
                            '-Werror', '-fsanitize=address,undefined', '-I', str(ROOT / 'src'),
                            str(ROOT / 'tests/native_files_host.c'), '-o', str(binary)], check=True)
            for case in (None, 'revision', 'identity', 'handle'):
                with self.subTest(case=case or 'ordinary'):
                    subprocess.run([str(binary)] + ([case] if case else []),
                                   env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1'), check=True)

    def test_large_profile_streaming(self):
        compiler = shutil.which('clang') or shutil.which('cc')
        self.assertIsNotNone(compiler, 'host C compiler required')
        with tempfile.TemporaryDirectory(prefix='baseos-native-files-large-') as directory:
            binary = pathlib.Path(directory) / 'native-files-large'
            subprocess.run([compiler, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra',
                            '-Werror', '-fsanitize=address,undefined', '-I', str(ROOT / 'src'),
                            str(ROOT / 'tests/native_files_large_host.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1'), check=True)


if __name__ == '__main__':
    unittest.main()
