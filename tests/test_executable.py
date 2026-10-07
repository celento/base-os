"""Ordinary BEX2 producer/parser contracts; no guest instructions execute.

Deterministic boundary inputs verify format arithmetic and unsupported build
features. This is not a fuzz or guest fault suite. Existing BEX1 bytes are frozen.
"""
import hashlib
import json
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


class ExecutableTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix='baseos-executable-')
        cls.directory = Path(cls.temporary.name)
        cls.env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1')
        for name in ('executable_host', 'executable_inspect_host'):
            subprocess.run([shutil.which('clang') or 'cc', '-std=c11', '-O1', '-g',
                '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                '-I', str(ROOT / 'src'), str(ROOT / 'tests' / (name + '.c')),
                str(ROOT / 'src/executable.c'), '-o', str(cls.directory / name)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def source(self, name, text):
        source = self.directory / (name + '.c')
        source.write_text(text)
        return source

    def compile(self, source, name, **kwargs):
        output, elf = self.directory / (name + '.bex'), self.directory / (name + '.elf')
        build(source, output, format='bex2', elf_output=elf, **kwargs)
        data = output.read_bytes()
        header = struct.unpack_from('<16I', data)
        self.assertEqual(header[4], len(data))
        plan = json.loads(subprocess.check_output([str(self.directory / 'executable_inspect_host'),
            str(output)], env=self.env, text=True))
        symbols = {}
        nm = os.environ.get('NM') or ('x86_64-elf-nm' if shutil.which('x86_64-elf-nm') else 'nm')
        for line in subprocess.check_output([nm, '-n', str(elf)], text=True).splitlines():
            fields = line.split()
            if len(fields) == 3:
                symbols[fields[2]] = int(fields[0], 16)
        self.assertEqual(header[5], symbols['_start'])
        self.assertEqual(symbols['__text_start'], 4096)
        self.assertEqual(header[6], symbols['__text_end'] - 4096)
        self.assertEqual(header[7], symbols['__data_start'])
        self.assertEqual(header[8], symbols['__data_file_end'] - header[7])
        self.assertEqual(header[9], symbols['__data_memory_end'] - header[7])
        self.assertEqual(header[10], symbols['__workspace_end'] - symbols['__workspace_start'])
        self.assertEqual(header[11], symbols['__stack_top'] - symbols['__stack_bottom'])
        self.assertEqual(plan['workspace'][0], symbols['__workspace_start'])
        self.assertEqual(plan['stack'][0], symbols['__stack_bottom'])
        self.assertEqual(plan['mapped_pages'] + 2, plan['owned_pages'])
        self.assertEqual(header[14:], (0, 0))
        self.assertEqual(data[64:4096], bytes(4096 - 64))
        return header, plan, data

    def test_production_parser(self):
        subprocess.run([str(self.directory / 'executable_host')], env=self.env, check=True)

    def test_small_initialized_and_large_bss_layouts(self):
        cases = (
            ('small', 'int main(void) { return 0; }', 0, 0),
            ('initialized', 'volatile unsigned seed=0x67452301; int main(void) { return (int)seed; }', 4, 4),
            ('large_bss', 'volatile unsigned seed=0x67452301; volatile char data[131077]; '
             'int main(void) { data[131076]=(char)seed; return data[0]; }', 4, 131081),
        )
        for name, text, minimum_file, minimum_mem in cases:
            with self.subTest(name=name):
                header, plan, data = self.compile(self.source(name, text), name, workspace_bytes=0)
                self.assertGreaterEqual(header[8], minimum_file)
                self.assertGreaterEqual(header[9], minimum_mem)
                if minimum_file:
                    self.assertEqual(data[header[7]:header[7]+4], b'\x01\x23\x45\x67')
                self.assertEqual(plan['workspace'][1:], [0, 0])
                if name == 'large_bss':
                    self.assertGreater(header[9], len(data))
                    self.assertGreater(plan['data'][2], 32)

    def test_workspace_and_stack_variants(self):
        source = self.source('variants', 'int main(void) { return 0; }')
        for workspace, stack in ((0, 16384), (1048576, 65536), (3145728, 262144),
                                 (4165632, 16384)):
            with self.subTest(workspace=workspace, stack=stack):
                header, plan, _ = self.compile(source, f'variants-{workspace}-{stack}',
                    workspace_bytes=workspace, stack_bytes=stack)
                self.assertEqual(header[10:12], (workspace, stack))
                self.assertEqual(plan['workspace'][2], workspace // 4096)
                self.assertEqual(plan['stack'][2], stack // 4096)
                if workspace == 4165632:
                    self.assertEqual(plan['owned_pages'], 1024)

    def test_workspace_examples(self):
        for name in ('workspace_array', 'workspace_index'):
            header, plan, _ = self.compile(ROOT / 'examples/c' / (name + '.c'), name)
            self.assertEqual(header[10:14], (1048576, 65536, 1, 1))
            self.assertEqual(plan['owned_pages'], 275)
            self.assertEqual(plan['data'][2], 0)

    def test_real_workspace_example_algorithms(self):
        data = document()
        input_path = self.directory / 'stats-sample.txt'
        input_path.write_bytes(data)
        seed = 0
        for byte in data:
            seed = (seed * 33 + byte) & 0xffffffff
        offsets = [0] if data else []
        offsets += [i + 1 for i, byte in enumerate(data[:-1]) if byte == 10]
        for name in ('workspace_array', 'workspace_index'):
            compiler = shutil.which('clang') or 'cc'
            flags = [compiler, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                     '-fsanitize=address,undefined', '-I', str(ROOT / 'sdk')]
            obj, exe = self.directory / (name + '.o'), self.directory / (name + '.host')
            subprocess.run(flags + ['-include', str(ROOT / 'tests/workspace_example_shim.h'),
                '-Dmain=workspace_example_main', '-c', str(ROOT / 'examples/c' / (name + '.c')),
                '-o', str(obj)], check=True)
            subprocess.run(flags + [str(ROOT / 'tests/workspace_example_host.c'), str(obj),
                '-o', str(exe)], check=True)
            for workspace in (1048576, 3145728):
                count = workspace // 4
                if name == 'workspace_array':
                    checksum = sum(((i * 1664525 + 1013904223) & 0xffffffff) ^ seed
                                   for i in range(count)) & 0xffffffff
                    expected = (f'BEX2 workspace array\nInput bytes: {len(data)}\nWords: {count}\n'
                                f'Seed: {seed}\nChecksum: {checksum}\n')
                else:
                    expected = (f'BEX2 workspace line index\nInput bytes: {len(data)}\nIndex capacity: {count}\n'
                                f'Lines: {len(offsets)}\nOffset checksum: {sum(offsets) & 0xffffffff}\n')
                report = self.directory / (name + '.expected.txt')
                report.write_text(expected)
                subprocess.run([str(exe), str(input_path), str(report), str(workspace)],
                               check=True, env=self.env)

    def test_default_bex1_matches_all_five_frozen_images(self):
        directory = ROOT / 'tests/fixtures/bex1-legacy'
        manifest = json.loads((directory / 'manifest.json').read_text())
        for name, entry in manifest['files'].items():
            output = self.directory / ('frozen-' + name)
            if name == 'hello.bex':
                subprocess.run(['nasm', '-f', 'bin', str(ROOT / 'examples/hello.asm'), '-o', str(output)], check=True)
            else:
                source = 'hello.c' if name == 'hello-c.bex' else name.replace('.bex', '.c')
                build(ROOT / 'examples/c' / source, output)
            data = output.read_bytes()
            self.assertEqual(data, (directory / name).read_bytes())
            self.assertEqual(hashlib.sha256(data).hexdigest(), entry['sha256'])

    def test_rejected_capacity_and_options_preserve_output(self):
        source = self.source('capacity', 'int main(void) { return 0; }')
        output = self.directory / 'preserved.bex'
        output.write_bytes(b'previous successful build')
        for options in ({'workspace_bytes': 1}, {'stack_bytes': 12288},
                        {'stack_bytes': 266240}, {'workspace_bytes': 4194304},
                        {'required_abi_minor': -1}):
            with self.subTest(options=options):
                with self.assertRaises((ValueError, subprocess.CalledProcessError)):
                    build(source, output, format='bex2', **options)
                self.assertEqual(output.read_bytes(), b'previous successful build')
        with self.assertRaises(ValueError):
            build(source, output, workspace_bytes=0)
        with self.assertRaises(ValueError):
            build(source, output, format='bex2', elf_output=output)
        self.assertEqual(output.read_bytes(), b'previous successful build')

    def test_unsupported_and_oversized_builds(self):
        cases = {
            'large_file': 'volatile unsigned char data[262144]={1}; int main(void) { return data[0]; }',
            'large_memory': 'volatile unsigned char data[4194304]; int main(void) { return data[0]; }',
            'tls': '__thread unsigned value; int main(void) { return (int)value; }',
            'constructors': 'volatile unsigned value; __attribute__((constructor)) void init(void) { value=1; } int main(void) { return value; }',
            'orphan': '__attribute__((section(".custom"))) volatile unsigned value=7; int main(void) { return value; }',
        }
        for name, text in cases.items():
            output = self.directory / (name + '.rejected.bex')
            with self.subTest(name=name):
                with self.assertRaises((ValueError, subprocess.CalledProcessError)):
                    build(self.source(name, text), output, format='bex2')
                self.assertFalse(output.exists())


if __name__ == '__main__':
    unittest.main()
