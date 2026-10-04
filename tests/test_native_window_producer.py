"""Explicit GUI producer and shared Pointer adapter; ordinary host execution only."""
import hashlib
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
from build_app import build, tool, _bex2_header


class NativeWindowProducerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='baseos-window-producer-')
        self.directory = Path(self.temporary.name)
        self.source = self.directory / 'example.c'
        self.source.write_text('''
#ifdef BOS_APP_NATIVE_WINDOW_V1
volatile unsigned launch_contract=0x57494e31;
#else
volatile unsigned launch_contract=0x484f5354;
#endif
int main(void) { return (int)launch_contract; }
''')

    def tearDown(self):
        self.temporary.cleanup()

    def test_explicit_gui_header_is_emitted_by_linker(self):
        output, elf = self.directory / 'gui.bex', self.directory / 'gui.elf'
        build(self.source, output, format='bex2', window='native-v1',
              workspace_bytes=0, stack_bytes=16384, elf_output=elf)
        data = output.read_bytes()
        header = _bex2_header(data)
        self.assertEqual(header[:4], (0x32584542, 64, 1, 1))
        self.assertEqual(header[10:14], (0, 16384, 1, 2))
        self.assertEqual(header[14:], (0, 0))
        self.assertEqual(struct.unpack_from('<I', data, header[7])[0], 0x57494e31)
        # Compare the real linked section, not just a Python-repacked header.
        linked_header = self.directory / 'linked-header.bin'
        subprocess.run([tool('objcopy'), '-O', 'binary', '-j', '.header',
                        str(elf), str(linked_header)], check=True)
        self.assertEqual(linked_header.read_bytes(), data[:64])

    def test_default_modes_keep_frozen_pointer_bytes_and_minor(self):
        # Original bytes independently built from d2da8bf before any GUI edits.
        expected = {
            'bex1': (4564, 'ef7bd7ab6a39e197808cac22e68951044d69527e96f76b5346cc4c7205b65248'),
            'bex2': (12288, 'c4f64a1805d1611f33d4c413ff0d5a6d48fd8968f0c04dcac78a77915ff2afd7'),
        }
        for format, (length, digest) in expected.items():
            with self.subTest(format=format):
                output = self.directory / ('pointer-' + format + '.bex')
                build(ROOT / 'examples/c/pointer.c', output, format=format)
                data = output.read_bytes()
                self.assertEqual(len(data), length)
                self.assertEqual(hashlib.sha256(data).hexdigest(), digest)
                if format == 'bex2':
                    header = _bex2_header(data)
                    self.assertEqual(header[3], 0)
                    self.assertEqual(header[10:14], (1048576, 65536, 1, 1))

    def test_gui_cli_and_make_output_have_explicit_bounded_contract(self):
        output = self.directory / 'cli.bex'
        subprocess.run([sys.executable, str(ROOT / 'tools/build_app.py'),
            str(self.source), str(output), '--format', 'bex2', '--window', 'native-v1',
            '--workspace-bytes', '0', '--stack-bytes', '16384', '--required-abi-minor', '3'], check=True)
        header = _bex2_header(output.read_bytes())
        self.assertEqual((header[3], header[13]), (1, 3))
        make_output = self.directory / 'make' / 'pointer-window.bex'
        subprocess.run(['make', 'OUT=' + str(make_output.parent), str(make_output)],
                       cwd=ROOT, check=True)
        header = _bex2_header(make_output.read_bytes())
        self.assertEqual(header[3], 1)
        self.assertEqual(header[10:14], (0, 16384, 1, 2))

    def test_incompatible_options_refuse_without_replacing_output(self):
        output = self.directory / 'preserved.bex'
        saved = b'previous successful build'
        output.write_bytes(saved)
        for options in (
            {'window': 'native-v1'},
            {'format': 'bex1', 'window': 'native-v1'},
            {'format': 'bex2', 'window': 'unknown'},
            {'format': 'bex2', 'window': 'native-v1', 'required_abi_minor': 0},
            {'format': 'bex2', 'window': 'native-v1', 'required_abi_minor': 1},
            {'format': 'bex2', 'window': 'native-v1', 'required_abi_minor': 0x100000000},
            {'format': 'bex2', 'window': 'native-v1', 'stack_bytes': 12288},
            {'format': 'bex2', 'window': 'native-v1', 'elf_output': output},
        ):
            with self.subTest(options=options), self.assertRaises(ValueError):
                build(self.source, output, **options)
            self.assertEqual(output.read_bytes(), saved)
        for options in (['--window', 'native-v1'],
                        ['--format', 'bex2', '--window', 'native-v1', '--required-abi-minor', '1']):
            result = subprocess.run([sys.executable, str(ROOT / 'tools/build_app.py'),
                str(self.source), str(output), *options], capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
            self.assertEqual(output.read_bytes(), saved)

    def test_validator_checks_flags_and_minimum_abi(self):
        output = self.directory / 'valid.bex'
        build(self.source, output, format='bex2', window='native-v1', workspace_bytes=0)
        data = output.read_bytes()
        for offset, value in ((12, 2), (12, 3), (52, 1)):
            changed = bytearray(data)
            struct.pack_into('<I', changed, offset, value)
            with self.subTest(offset=offset, value=value), self.assertRaises(ValueError):
                _bex2_header(changed)

    def test_gui_runs_the_same_pointer_model_and_event_loop(self):
        output = self.directory / 'pointer-window-host'
        subprocess.run([shutil.which('clang') or 'cc', '-std=c11', '-O1', '-g',
            '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
            '-DBOS_APP_NATIVE_WINDOW_V1=1', '-I', str(ROOT / 'sdk'),
            str(ROOT / 'tests/pointer_example_host.c'), '-o', str(output)], check=True)
        subprocess.run([str(output)], check=True, env=dict(os.environ,
            ASAN_OPTIONS='detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1'))


if __name__ == '__main__':
    unittest.main()
