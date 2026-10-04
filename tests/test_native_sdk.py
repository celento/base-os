"""Focused valid C SDK images, exact geometry and example fixture checks."""
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from build_app import build
from make_stats_fixture import document


class NativeSdkTests(unittest.TestCase):
    def test_supported_canvas_lifecycle(self):
        compiler = shutil.which('clang') or shutil.which('cc')
        with tempfile.TemporaryDirectory(prefix='baseos-sdk-host-') as directory:
            exe = Path(directory) / 'canvas'
            subprocess.run([compiler, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra',
                            '-Werror', '-fsanitize=address,undefined', '-I', str(ROOT / 'src'),
                            str(ROOT / 'tests/sdk_canvas_host.c'), str(ROOT / 'tests/net_stub.c'),
                            str(ROOT / 'src/download.c'), '-DDOWNLOAD_HOST_TEST',
                            '-o', str(exe)], check=True)
            env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0')
            subprocess.run([str(exe)], env=env, check=True)

    def test_existing_and_large_c_images(self):
        with tempfile.TemporaryDirectory(prefix='baseos-sdk-build-') as directory:
            for source in ('examples/c/hello.c', 'examples/c/notebook.c',
                           'examples/c/counter.c', 'examples/c/docstats.c',
                           'tests/sdk_stream_app.c', 'tests/sdk_capacity_app.c'):
                output = Path(directory) / (Path(source).stem + '.bex')
                build(ROOT / source, output)
                data = output.read_bytes()
                magic, entry, length, reserved = struct.unpack_from('<4I', data)
                self.assertEqual(magic, 0x31584542)
                self.assertEqual(length, len(data))
                self.assertEqual(reserved, 0)
                self.assertTrue(16 <= entry < length <= 49152)
                if source.endswith('sdk_stream_app.c'):
                    self.assertGreater(length, 32768)

    def test_document_fixture(self):
        data = document()
        self.assertEqual(len(data), 56812)
        self.assertEqual(len(data.split()), 9021)
        self.assertEqual(len(data.splitlines()), 904)
        self.assertFalse(data.endswith(b'\n'))
        self.assertGreater(len(data), 16383)
        self.assertLess(len(data), 65536)
        self.assertEqual(data, document())


if __name__ == '__main__':
    unittest.main()
