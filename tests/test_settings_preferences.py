"""Ordinary Settings workflows using extracted production preference helpers."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_editor_binding import function

ROOT = Path(__file__).resolve().parents[1]


class SettingsPreferenceTests(unittest.TestCase):
    def test_pending_preferences_durability_destinations_and_retry(self):
        source = (ROOT / 'src/kernel.c').read_text()
        first = source.index('enum { PREF_THEME, PREF_SAVER, PREF_DISPLAY, PREF_COUNT };')
        last = source.index('static void theme_set(int id) {', first)
        implementation = source[first:last]
        for name in ('display_request', 'display_revert', 'display_keep', 'display_load'):
            implementation += function(source, name)
        with tempfile.TemporaryDirectory(prefix='baseos-settings-') as temporary:
            directory = Path(temporary)
            (directory / 'settings_kernel.inc').write_text(implementation)
            binary = directory / 'settings-host'
            compiler = shutil.which('clang') or shutil.which('cc')
            subprocess.run([compiler, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                            '-fsanitize=address,undefined', '-I', str(ROOT / 'src'), '-I', str(directory),
                            str(ROOT / 'tests/settings_preferences_host.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True,
                           env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1'))


if __name__ == '__main__':
    unittest.main()
