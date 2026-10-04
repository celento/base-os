"""Opt-in profile checks on disposable images and ordinary supported operations."""
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from test_volume import VolumeHelpers, ROOT, volume
from init_data import initialize
from migrate_volume import migrate


def fixture(nodes, version=5):
    layout = volume.LARGE_DATA_LAYOUT if version == 5 else volume.DATA_LAYOUT if version == 4 else volume.FLOPPY_LAYOUT
    raw = bytearray(layout.sectors * 512)
    if version >= 4:
        raw[:512] = volume.data_marker('large' if version == 5 else 'default')
    payload = bytearray()
    for ident, n in sorted(nodes.items()):
        payload += struct.pack('<HhBBHI24s', ident, n['parent'], n['directory'], n['app'], 0,
                               len(n['data']), n['name'].encode('latin1'))
        if version >= 3:
            payload += struct.pack('<I', n['modified'])
        payload += n['data']
    header = struct.pack('<6I', volume.MAGIC, version, len(nodes), len(payload),
                         volume.rolling(payload) if version == 1 else zlib.crc32(payload), 23)
    header += struct.pack('<I', 0 if version == 1 else zlib.crc32(header))
    start = layout.lbas[0] * 512
    raw[start:start + 512] = header.ljust(512, b'\0')
    raw[start + 512:start + 512 + len(payload)] = payload
    return raw


def node(name='', parent=-1, directory=1, app=0, data=b'', modified=0):
    return dict(name=name, parent=parent, directory=directory, app=app, data=data, modified=modified)


class LargeVolumeTests(VolumeHelpers, unittest.TestCase):
    def test_default_and_explicit_creation_preserve_existing_bytes(self):
        header = struct.pack('<6I', 0x44534f42, 1, 32768, 16383, 1, 16384)
        self.assertEqual(volume.data_marker(), (header + struct.pack('<I', zlib.crc32(header))).ljust(512, b'\0'))
        self.assertEqual(volume.DATA_LAYOUT.capacity(64), 8385024)
        self.assertEqual(volume.DATA_LAYOUT.capacity(256), 8377344)
        self.assertEqual(volume.LARGE_DATA_LAYOUT.capacity(64), 33550848)
        self.assertEqual(volume.LARGE_DATA_LAYOUT.capacity(256), 33543168)
        with tempfile.TemporaryDirectory() as temporary:
            d = pathlib.Path(temporary)
            small, large = d / 'small.img', d / 'large.img'
            self.assertTrue(initialize(small))
            before = small.read_bytes()
            self.assertFalse(initialize(small))
            with self.assertRaises(ValueError):
                initialize(small, profile='large')
            self.assertEqual(small.read_bytes(), before)
            self.assertTrue(initialize(large, profile='large'))
            before = large.read_bytes()
            self.assertEqual(len(before), 64 * 1048576)
            self.assertEqual(volume.disk_layout(before), volume.LARGE_DATA_LAYOUT)
            self.assertEqual(before[:512], volume.data_marker('large'))
            self.assertFalse(any(before[512:]))
            self.assertFalse(initialize(large, profile='large'))
            with self.assertRaises(ValueError):
                initialize(large)
            self.assertEqual(large.read_bytes(), before)
            self.assertIn('16,777,216 bytes', self.cli(large, 'info'))
            self.assertFalse(list(d.glob('*.bak')))

    def test_copy_migration_preserves_legacy_nodes_and_new_destination(self):
        nodes = {0: node(), 7: node('Projects', 0),
                 9: node('bytes-\xe9', 7, 0, data=b'\0\xffexact\r\n', modified=345678),
                 10: node('App', 7, 0, 1, modified=987654),
                 21: node('Empty', 7, 0, modified=87654)}
        with tempfile.TemporaryDirectory() as temporary:
            d = pathlib.Path(temporary)
            for version in (1, 2, 3, 4, 5):
                source, destination = d / f'source-{version}', d / f'new-{version}'
                before = fixture(nodes, version)
                if version == 1:
                    before = before[:2880 * 512]  # Old 1.44 MiB source, copied read-only.
                source.write_bytes(before)
                original_nodes = volume.load(before)[2]
                self.assertEqual(migrate(source, destination), (len(nodes), 9))
                after = destination.read_bytes()
                self.assertEqual(source.read_bytes(), before)
                self.assertEqual(volume.disk_layout(after), volume.LARGE_DATA_LAYOUT)
                self.assertEqual(volume.load(after)[2], original_nodes)
                for slot in (0, 1):
                    self.assertEqual(volume.decode(after, slot)[1], original_nodes)
                with self.assertRaises(FileExistsError):
                    migrate(source, destination)
                self.assertEqual(destination.read_bytes(), after)
            self.assertFalse(list(d.glob('*.bak')))

    def test_large_host_exchange_and_capacity_rejections_are_atomic(self):
        with tempfile.TemporaryDirectory() as temporary:
            d = pathlib.Path(temporary)
            image, host, exported = d / 'image', d / 'host', d / 'export'
            original = fixture({0: node(), 1: node('keep', 0, 0, data=b'exact', modified=123)})
            image.write_bytes(original)
            content = bytes(range(256)) * (volume.LARGE_DATA_FILE_LIMIT // 256)
            host.write_bytes(content)
            self.cli(image, 'import', host, '/maximum')
            after = image.read_bytes()
            self.cli(image, 'export', '/maximum', exported)
            self.assertEqual(exported.read_bytes(), content)
            self.assertEqual(volume.decode(after, 0), volume.decode(original, 0))
            self.assertEqual(next(d.glob('*.bak')).read_bytes(), original)
            self.assertEqual(after[:512], volume.data_marker('large'))
            self.assertEqual(after[-512:], original[-512:])
            before, backups = after, set(d.glob('*.bak'))
            host.write_bytes(content + b'x')
            self.cli(image, 'import', host, '/maximum', '--replace', ok=False)
            self.assert_unchanged(image, before, backups)
            slot, generation, nodes = volume.load(before)
            remaining = volume.LARGE_DATA_LAYOUT.capacity(256) - len(content) - 5
            nodes[3] = node('remainder', 0, 0, data=content[:remaining], modified=456)
            for ident in range(4, 256):
                nodes[ident] = node(f'empty{ident}', 0, 0)
            volume.commit(image, before, slot, generation, nodes)
            full = image.read_bytes()
            self.assertEqual(volume.load(full)[2], nodes)
            self.assertIn('Remaining file-data allowance: 0 bytes', self.cli(image, 'info'))
            self.assertIn('Total file-data limit: 33,543,168 bytes', self.cli(image, 'info'))
            slot, generation, nodes = volume.load(full)
            nodes[3]['data'] += b'x'
            backups = set(d.glob('*.bak'))
            with self.assertRaisesRegex(ValueError, 'Volume full'):
                volume.commit(image, full, slot, generation, nodes)
            self.assert_unchanged(image, full, backups)
            # An explicit conversion to smaller geometry also preflights capacity.
            with self.assertRaisesRegex(ValueError, 'File exceeds'):
                migrate(image, d / 'too-small', profile='default')
            self.assertFalse((d / 'too-small').exists())
            self.assertEqual(image.read_bytes(), full)

    def test_migration_and_initializer_reject_locked_inputs(self):
        with tempfile.TemporaryDirectory() as temporary:
            d = pathlib.Path(temporary)
            image, output = d / 'source', d / 'new'
            before = fixture({0: node()})
            image.write_bytes(before)
            child = subprocess.Popen([sys.executable, '-c',
                'import fcntl,sys; f=open(sys.argv[1],"r+b"); '
                'fcntl.lockf(f,fcntl.LOCK_EX); print("locked",flush=True); sys.stdin.read()',
                str(image)], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
            try:
                self.assertEqual(child.stdout.readline().strip(), b'locked')
                with self.assertRaises(BlockingIOError):
                    migrate(image, output)
                with self.assertRaises(BlockingIOError):
                    initialize(image, profile='large')
                self.assertFalse(output.exists())
                self.assertEqual(image.read_bytes(), before)
            finally:
                child.communicate(timeout=5)

    def test_native_large_operations_and_exact_e820_ranges(self):
        compiler = shutil.which('clang') or shutil.which('cc')
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as temporary:
            d = pathlib.Path(temporary)
            poll = d / 'poll.c'
            poll.write_text('unsigned large_test_polls; void fs_background_poll(void) { ++large_test_polls; }\n')
            for name in ('large_volume_host', 'optional_memory_host'):
                binary = d / name
                extras = [str(poll)] if name == 'large_volume_host' else []
                subprocess.run([compiler, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                                '-fsanitize=address,undefined', '-I', str(ROOT / 'src'),
                                str(ROOT / 'tests' / f'{name}.c'), *extras, '-o', str(binary)], check=True)
                args = [str(d / 'native.img')] if name == 'large_volume_host' else []
                subprocess.run([str(binary), *args], check=True)
            raw = (d / 'native.img').read_bytes()
            layout = volume.disk_layout(raw)
            self.assertEqual(layout, volume.LARGE_DATA_LAYOUT)
            self.assertEqual(raw[:512], volume.data_marker('large'))
            self.assertEqual(raw[-512:], bytes(512))
            for slot in (0, 1):
                start = layout.lbas[slot] * 512
                magic, version, count, size, crc, _, hcrc = struct.unpack_from('<7I', raw, start)
                self.assertEqual((magic, version, count, size), (volume.MAGIC, 5, 256, 33553408))
                self.assertEqual(zlib.crc32(raw[start:start + 24]), hcrc)
                self.assertEqual(zlib.crc32(raw[start + 512:start + 512 + size]), crc)
                _, nodes = volume.decode(raw, slot)
                self.assertEqual(sum(len(n['data']) for n in nodes.values()), 33543168)
                for ident in (1, 2):
                    content = nodes[ident]['data']
                    for offset in range(0, len(content), 65536):
                        sample = bytes((i * 37 + (offset >> 16) + 19) & 255 for i in range(256)) * 256
                        self.assertEqual(content[offset:offset + 65536], sample[:min(65536, len(content) - offset)])


if __name__ == '__main__':
    unittest.main()
