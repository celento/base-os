"""Ordinary reference images, independently decoded by Pillow at fixture creation.
Normal test execution requires no Pillow and does not generate adversarial files.
"""
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
FIXTURES = ROOT / 'tests/fixtures/images'


class ImageFormatTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix='baseos-image-host-')
        cls.directory = pathlib.Path(cls.temporary.name)
        cls.compiler = shutil.which('clang') or shutil.which('cc')
        cls.manifest = json.loads((FIXTURES / 'manifest.json').read_text())
        cls.environment = dict(os.environ, ASAN_OPTIONS=os.environ.get('ASAN_OPTIONS', 'detect_leaks=0'),
                               UBSAN_OPTIONS='halt_on_error=1')
        for name in ('image_decode_host', 'image_viewer_host'):
            subprocess.run([cls.compiler, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-I', str(ROOT / 'src'),
                            str(ROOT / 'tests' / f'{name}.c'), str(ROOT / 'src/image_decode.c'),
                            '-o', str(cls.directory / name)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def decode(self, fixture, capacity=None):
        output = self.directory / 'decoded.raw'
        command = [str(self.directory / 'image_decode_host'), str(FIXTURES / fixture), str(output)]
        if capacity is not None:
            command.append(str(capacity))
        subprocess.run(command, check=True, env=self.environment)
        header, data = output.read_bytes().split(b'\n', 1)
        return tuple(int(value) for value in header.split()), data

    def test_reference_pixels(self):
        for fixture in self.manifest:
            with self.subTest(image=fixture['name']):
                source = (FIXTURES / fixture['name']).read_bytes()
                self.assertEqual(hashlib.sha256(source).hexdigest(), fixture['sha256'])
                (error, width, height, channels, image_format, peak), actual = self.decode(fixture['name'])
                self.assertEqual(error, 0)
                self.assertEqual((width, height, image_format),
                                 (fixture['width'], fixture['height'], fixture['format']))
                self.assertGreater(peak, 0)
                self.assertLessEqual(peak, 0x640000)
                expected = zlib.decompress((FIXTURES / fixture['reference']).read_bytes())
                if not channels:
                    self.assertEqual(actual, expected)
                    continue
                rgba = bytearray()
                for start in range(0, len(actual), channels):
                    pixel = actual[start:start + channels]
                    rgb = pixel[:3] if channels >= 3 else pixel[:1] * 3
                    alpha = pixel[-1] if channels in (2, 4) else 255
                    rgba.extend(rgb); rgba.append(alpha)
                self.assertEqual(len(rgba), len(expected))
                if fixture['format'] == 2:  # independent integer JPEG IDCT rounding
                    distances = [abs(a - b) for a, b in zip(rgba, expected)]
                    self.assertLessEqual(max(distances), 3)
                    self.assertLess(sum(distances) / len(distances), 0.15)
                else:
                    if fixture['format'] == 5:
                        # GIF decoders need not preserve palette RGB beneath a
                        # completely transparent pixel. Visible RGBA must agree.
                        expected = bytearray(expected)
                        for i in range(0, len(rgba), 4):
                            if not rgba[i + 3]: rgba[i:i + 3] = bytes(3)
                            if not expected[i + 3]: expected[i:i + 3] = bytes(3)
                    self.assertEqual(rgba, expected)

    def test_bounded_workspace(self):
        # A real valid photograph-style JPEG gets a clean error in a small arena.
        header, data = self.decode('landscape.jpg', 32768)
        self.assertEqual(header[0], -4)
        self.assertFalse(data)
        header, _ = self.decode('landscape.jpg')
        self.assertEqual(header[0], 0)

    def test_viewer_normal_flows(self):
        subprocess.run([str(self.directory / 'image_viewer_host'), str(FIXTURES / 'landscape.png')],
                       check=True, env=self.environment)


if __name__ == '__main__':
    unittest.main()
