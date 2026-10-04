"""Real process lifecycle/scheduling with deterministic ordinary host callbacks."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from process_host_extract import extract

ROOT = Path(__file__).resolve().parents[1]


class ProcessLifetimeTests(unittest.TestCase):
    def test_production_create_schedule_stop_reap(self):
        with tempfile.TemporaryDirectory(prefix='baseos-process-lifetime-') as temporary:
            directory = Path(temporary)
            extract((ROOT / 'src/process.c').read_text(), directory, 'process_lifetime', scheduler=True)
            executable = directory / 'process-lifetime'
            subprocess.run([shutil.which('clang') or 'cc', '-std=gnu11', '-O1', '-g',
                '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                '-fsanitize=address,undefined', '-I', str(ROOT / 'src'), '-I', str(directory),
                str(ROOT / 'tests/process_lifetime_host.c'),
                str(ROOT / 'src/physmem.c'), str(ROOT / 'src/bootinfo.c'), '-o', str(executable)], check=True)
            for ram in (64, 128):
                subprocess.run([str(executable), str(ram)], check=True, env=dict(os.environ,
                    ASAN_OPTIONS='detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1'))


if __name__ == '__main__':
    unittest.main()
