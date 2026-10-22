"""Ordinary Calendar UI workflows with the real model and software renderer."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class CalendarUITests(unittest.TestCase):
    def test_smallest_production_display_fits(self):
        theme = (ROOT / 'src/ui/theme.c').read_text()
        header = (ROOT / 'src/apps/calendar.h').read_text()
        cal_h = int(re.search(r'^#define\s+CAL_H\s+(\d+)', header, re.M)[1])
        # Each style's top bar, taskbar or dock (Modern reserves 60 px) and window chrome.
        for name in ('classic_metrics', 'modern_metrics'):
            block = re.search(name + r'\s*=\s*\{(.*?)\};', theme, re.S)[1]
            value = lambda key: int(re.search(r'\.' + key + r'\s*=\s*(\d+)', block)[1])
            taskbar = 60 if 'MODERN' in block else value('taskbar_h')
            title = value('border') + value('title_h') + value('menu_h') + (0 if value('menu_h') else 1)
            available = 600 - value('topbar_h') - taskbar - 8
            with self.subTest(style=name):
                self.assertLessEqual(cal_h + title, available)

    def test_calendar_mouse_keyboard_recovery_and_rendering(self):
        with tempfile.TemporaryDirectory(prefix='baseos-calendar-ui-') as temporary:
            binary = Path(temporary) / 'calendar-ui'
            compiler = shutil.which('cc')
            self.assertIsNotNone(compiler)
            subprocess.run([compiler, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O1',
                            '-DRTC_HOST_TEST', '-I', str(ROOT / 'src'),
                            str(ROOT / 'tests/calendar_ui_host.c'), str(ROOT / 'src/kernel/rtc.c'),
                            '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], check=True, text=True, capture_output=True)
            self.assertIn('All Calendar UI functional checks passed.', result.stdout)
            for section in ('creation', 'recovery', 'validation', 'safety', 'storage',
                            'navigation', 'rendering'):
                self.assertIn(f'Calendar {section}:', result.stdout)


if __name__ == '__main__':
    unittest.main()
