"""BaseFS 2 core: format, mount, data round trips, free space, corruption
detection and a power cut at every sector of a commit, on a RAM disk."""
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class BaseFsTests(unittest.TestCase):
    def test_core(self):
        compiler = shutil.which('clang') or shutil.which('cc')
        self.assertIsNotNone(compiler, 'host C compiler required')
        with tempfile.TemporaryDirectory(prefix='baseos-basefs-') as directory:
            executable = pathlib.Path(directory) / 'basefs_host'
            subprocess.run([compiler, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-I', str(ROOT / 'src'),
                            str(ROOT / 'tests' / 'basefs_host.c'), '-o', str(executable)],
                           check=True)
            env = dict(os.environ)
            env['UBSAN_OPTIONS'] = 'halt_on_error=1'
            subprocess.run([str(executable)], check=True, env=env)


if __name__ == '__main__':
    unittest.main()
