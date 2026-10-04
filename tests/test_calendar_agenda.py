"""Ordinary deterministic Calendar model/storage workflows with the real FS."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class CalendarAgendaTests(unittest.TestCase):
    def test_model_persistence_and_recovery(self):
        with tempfile.TemporaryDirectory(prefix='baseos-calendar-') as temporary:
            binary = Path(temporary) / 'calendar-host'
            compiler = shutil.which('clang') or shutil.which('cc')
            subprocess.run([compiler, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                            '-fsanitize=address,undefined', '-I', str(ROOT / 'src'),
                            str(ROOT / 'tests/calendar_agenda_host.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True,
                           env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1'))


if __name__ == '__main__':
    unittest.main()
