"""Ordinary Settings workflows using extracted production preference helpers."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from desktop_source import block
from test_editor_binding import function

ROOT = Path(__file__).resolve().parents[1]


class SettingsPreferenceTests(unittest.TestCase):
    def test_pending_preferences_durability_destinations_and_retry(self):
        source = (ROOT / 'src/shell/desktop/desktop.c').read_text()
        # The globals every preference loads into, with their defaults.
        globals_ = block(source, '/* Appearance preferences. */', 'static const char *session_status',
                         include_end=False)
        # The preference table, pending states, read/write/tick/retry/shutdown,
        # loaders, savers and the appearance setters, then the display
        # request/revert/keep confirmation and its loader.
        implementation = block(source, 'enum {\n    PREF_STYLE, PREF_ACCENT,', '/* ---------- About ---------- */',
                               include_end=False)
        for name in ('display_request', 'display_revert', 'display_keep', 'display_load'):
            implementation += function(source, name)
        with tempfile.TemporaryDirectory(prefix='baseos-settings-') as temporary:
            directory = Path(temporary)
            (directory / 'preference_globals.inc').write_text(globals_)
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
