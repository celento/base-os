import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]

class AudioTests(unittest.TestCase):
    def run_host(self, name):
        compiler = shutil.which('clang') or shutil.which('cc')
        with tempfile.TemporaryDirectory() as temp:
            exe = pathlib.Path(temp) / name
            subprocess.run([compiler, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra',
                            '-Werror', '-fsanitize=address,undefined', '-I', str(ROOT / 'src'),
                            str(ROOT / 'tests' / (name + '.c')), str(ROOT / 'src/media.c'),
                            '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
    def test_wave(self): self.run_host('media_host')
    def test_driver(self): self.run_host('audio_host')

if __name__ == '__main__': unittest.main()
