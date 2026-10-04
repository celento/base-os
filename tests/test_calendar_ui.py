"""Ordinary Calendar UI workflows with the real model and software renderer."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class CalendarUITests(unittest.TestCase):
    def test_calendar_mouse_keyboard_recovery_and_rendering(self):
        with tempfile.TemporaryDirectory(prefix='baseos-calendar-ui-') as temporary:
            binary = Path(temporary) / 'calendar-ui'
            compiler = shutil.which('cc')
            self.assertIsNotNone(compiler)
            subprocess.run([compiler, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O1',
                            '-DRTC_HOST_TEST', '-I', str(ROOT / 'src'),
                            str(ROOT / 'tests/calendar_ui_host.c'), str(ROOT / 'src/rtc.c'),
                            '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], check=True, text=True, capture_output=True)
            self.assertIn('All Calendar UI functional checks passed.', result.stdout)
            for section in ('creation', 'recovery', 'validation', 'safety', 'storage',
                            'navigation', 'rendering'):
                self.assertIn(f'Calendar {section}:', result.stdout)


if __name__ == '__main__':
    unittest.main()
