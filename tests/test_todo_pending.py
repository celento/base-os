"""Todo deferred-save lifecycle with the actual filesystem under ASan/UBSan."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class TodoPendingTests(unittest.TestCase):
    def test_coalescing_errors_shutdown_and_durability(self):
        with tempfile.TemporaryDirectory(prefix='baseos-todo-') as temporary:
            binary = Path(temporary) / 'todo-host'
            compiler = shutil.which('clang') or shutil.which('cc')
            subprocess.run([compiler, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                            '-fsanitize=address,undefined', '-I', str(ROOT / 'src'),
                            str(ROOT / 'tests/todo_pending_host.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True,
                           env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1'))


if __name__ == '__main__':
    unittest.main()
