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

ROOT = Path(__file__).resolve().parents[1]


class NativePlatformTests(unittest.TestCase):
    def test_dispatch_and_lifetimes(self):
        source = (ROOT / 'src/process.c').read_text()
        first = source.index('#define TASK_KEYS ')
        types = source[first:source.index('static NativeTask *tasks=', first)]
        exported = {'process_interrupt': 'int', 'process_task_start_with_arg': 'int',
                    'process_task_stop': 'void', 'process_task_clear': 'void',
                    'process_task_key': 'int'}
        for name, result in exported.items():
            source = source.replace(result + ' ' + name + '(', 'static ' + result + ' ' + name + '(')
        names = ('allocate_owner', 'current_owner', 'release_owner', 'task_release',
                 'task_at', 'valid_image', 'process_task_start_with_arg', 'task_wake',
                 'process_task_key', 'process_task_stop', 'process_task_clear',
                 'task_suspend', 'finish', 'user_range', 'user_path',
                 'abi_query', 'native_file_call', 'process_interrupt')
        code = ''.join(function(source, name) for name in names)
        for name, result in exported.items():
            code = code.replace('static ' + result + ' ' + name + '(', result + ' ' + name + '(')
        for old, new in (
            ('__asm__ volatile("pushfl; popl %0; sti":"=r"(flags)::"memory");', 'flags=0;'),
            ('__asm__ volatile("pushl %0; popfl"::"r"(flags):"memory","cc");', '(void)flags;'),
        ):
            self.assertEqual(code.count(old), 1)
            code = code.replace(old, new)
        self.assertNotIn('__asm__', code)
        with tempfile.TemporaryDirectory(prefix='baseos-platform-dispatch-') as temporary:
            directory = Path(temporary)
            (directory / 'native_platform_types.inc').write_text(types)
            (directory / 'native_platform_ops.inc').write_text(code)
            executable = directory / 'dispatch'
            subprocess.run([shutil.which('clang') or 'cc', '-std=gnu11', '-O1', '-g',
                            '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
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
