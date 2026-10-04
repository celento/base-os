import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
class VideoDrawTests(unittest.TestCase):
    def test_fit_render(self):
        with tempfile.TemporaryDirectory() as directory:
            executable=pathlib.Path(directory)/'video-draw'
            subprocess.run([shutil.which('clang') or 'cc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror',
                            '-fsanitize=address,undefined','-I',str(ROOT/'src'),str(ROOT/'tests/video_draw_host.c'),
                            '-o',str(executable)],check=True)
            env=dict(os.environ);env['ASAN_OPTIONS']='detect_leaks=0';env['UBSAN_OPTIONS']='halt_on_error=1'
            subprocess.run([str(executable)],check=True,env=env)
if __name__=='__main__':unittest.main()
