import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
class PlayerTests(unittest.TestCase):
    def test_client(self):
        with tempfile.TemporaryDirectory() as temp:
            output=pathlib.Path(temp)/'player'
            subprocess.run([shutil.which('clang') or 'cc','-std=gnu11','-Wall','-Wextra','-Werror',
                '-O1','-g','-fsanitize=address,undefined','-I',str(ROOT/'src'),
                str(ROOT/'tests/player_host.c'),'-o',str(output)],check=True)
            env=dict(os.environ);env['ASAN_OPTIONS']='detect_leaks=0'
            subprocess.run([str(output)],env=env,check=True)
if __name__=='__main__':unittest.main()
