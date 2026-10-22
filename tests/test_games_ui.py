"""Clock and the games with the real kit and renderer, in both styles."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class GamesUITests(unittest.TestCase):
    def test_layouts_fit_and_clicks_hit_drawn_controls(self):
        with tempfile.TemporaryDirectory(prefix='baseos-games-ui-') as temporary:
            binary = Path(temporary) / 'games-ui'
            compiler = shutil.which('cc')
            self.assertIsNotNone(compiler)
            subprocess.run([compiler, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O1',
                            '-DRTC_HOST_TEST', '-I', str(ROOT / 'src'),
                            str(ROOT / 'tests/games_ui_host.c'), str(ROOT / 'src/kernel/rtc.c'),
                            '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], check=True, text=True, capture_output=True)
            self.assertIn('All games UI checks passed.', result.stdout)
            for style in ('Classic', 'Modern'):
                self.assertIn(f'Games {style}:', result.stdout)


if __name__ == '__main__':
    unittest.main()
