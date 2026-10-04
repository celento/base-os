"""Ordinary additive ABI negotiation, dispatch, wait and lifecycle contracts.

No native machine code or hardware instructions run in the host fixture. The
production functions are extracted without rewriting their logic; only the two
privileged interrupt-flag instructions around legacy sync use the existing shim.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

from test_editor_binding import function
from process_host_extract import extract

ROOT = Path(__file__).resolve().parents[1]


class NativePlatformTests(unittest.TestCase):
    def test_dispatch_and_lifetimes(self):
        source = (ROOT / 'src/process.c').read_text()
        with tempfile.TemporaryDirectory(prefix='baseos-platform-dispatch-') as temporary:
            directory = Path(temporary)
            extract(source, directory, 'native_platform')
            executable = directory / 'dispatch'
            subprocess.run([shutil.which('clang') or 'cc', '-std=gnu11', '-O1', '-g',
                            '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-fsanitize=address,undefined',
                            '-I', str(ROOT / 'src'), '-I', str(directory),
                            str(ROOT / 'tests/native_platform_dispatch_host.c'),
                            '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True, env=dict(os.environ,
                ASAN_OPTIONS='detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1'))

    def test_frozen_old_executable_bytes(self):
        directory = ROOT / 'tests/fixtures/bex1-hour05'
        manifest = json.loads((directory / 'manifest.json').read_text())
        self.assertEqual(manifest['source_revision'], 'a7ea36be6db5ae2cd477dfe36e8e58db48145048')
        for name, entry in manifest['files'].items():
            data = (directory / name).read_bytes()
            self.assertEqual(len(data), entry['bytes'])
            self.assertEqual(hashlib.sha256(data).hexdigest(), entry['sha256'])
            magic, start, size, reserved = struct.unpack_from('<4I', data)
            self.assertEqual(magic, 0x31584542)
            self.assertEqual(size, len(data))
            self.assertEqual(reserved, 0)
            self.assertTrue(16 <= start < size <= 49152)

    def test_sdk_compatibility_helper(self):
        sdk = (ROOT / 'sdk/baseos.h').read_text()
        code = function(sdk, 'bos_sync_compatible')
        with tempfile.TemporaryDirectory(prefix='baseos-platform-helper-') as temporary:
            directory = Path(temporary)
            (directory / 'native_platform_helper.inc').write_text(code)
            executable = directory / 'helper'
            subprocess.run([shutil.which('clang') or 'cc', '-std=c11', '-O1', '-Wall',
                '-Wextra', '-Werror', '-fsanitize=address,undefined', '-I', str(directory),
                str(ROOT / 'tests/native_platform_helper_host.c'), '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True, env=dict(os.environ,
                ASAN_OPTIONS='detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1'))

if __name__ == '__main__':
    unittest.main()
