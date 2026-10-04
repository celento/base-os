import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class LargeAudioTests(unittest.TestCase):
    def test_owned_profile_boundaries_and_complete_mp3(self):
        if not shutil.which('ffmpeg'):
            self.skipTest('ffmpeg needed for original Harbor-derived MP3')
        with tempfile.TemporaryDirectory() as temp:
            directory = pathlib.Path(temp)
            song = directory / 'harbor-three-minutes.mp3'
            subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y',
                            '-stream_loop', '-1', '-i', str(ROOT / 'assets/examples/harbor.mp3'),
                            '-t', '180', '-codec:a', 'libmp3lame', '-b:a', '192k',
                            '-write_xing', '0', '-id3v2_version', '0', str(song)], check=True)
            self.assertGreater(song.stat().st_size, 2 * 1024 * 1024)
            mono = directory / 'different-rate-mono.mp3'
            subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y',
                            '-i', str(ROOT / 'assets/examples/harbor.mp3'), '-t', '0.2',
                            '-ac', '1', '-ar', '22050', '-codec:a', 'libmp3lame', '-b:a', '64k',
                            '-write_xing', '0', '-id3v2_version', '0', str(mono)], check=True)
            exe = directory / 'audio-large-host'
            compiler = shutil.which('clang') or shutil.which('cc')
            subprocess.run([compiler, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra',
                            '-Werror', '-fsanitize=address,undefined', '-DMEDIA_MP3_HOST_TEST',
                            '-I', str(ROOT / 'src'), str(ROOT / 'tests/audio_large_host.c'),
                            str(ROOT / 'src/media.c'), str(ROOT / 'src/media_mp3.c'),
                            '-o', str(exe)], check=True)
            subprocess.run([str(exe), str(song), str(mono)], check=True,
                           env=dict(os.environ, UBSAN_OPTIONS='halt_on_error=1'))


if __name__ == '__main__':
    unittest.main()
