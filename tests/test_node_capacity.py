"""Ordinary supported filesystem growth and old-volume compatibility checks."""
import pathlib
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib
from test_volume import VolumeHelpers, ROOT, volume


class NodeCapacityTests(VolumeHelpers, unittest.TestCase):
    def test_native_capacity_and_paths(self):
        compiler = shutil.which('clang') or shutil.which('cc')
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as temporary:
            binary = pathlib.Path(temporary) / 'node-capacity'
            subprocess.run([compiler, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-I', str(ROOT / 'src'),
                            str(ROOT / 'tests/node_capacity_host.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_old_full_64_record_data_volume(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            image, host, exported = [directory / name for name in ('image', 'host', 'exported')]
            chunk = bytes(range(256)) * (volume.DATA_FILE_LIMIT // 256)
            final = chunk[:volume.DATA_TOTAL_LIMIT - len(chunk) * 3]
            files = [(f'large{i}', chunk if i < 3 else final) for i in range(4)]
            files += [(f'empty{i}', b'') for i in range(59)]
            original = self.fixture(4, files=files)
            image.write_bytes(original)
            self.assertEqual(len(volume.load(original)[2]), 64)
            self.assertIn('Remaining file-data allowance: 0 bytes', self.cli(image, 'info'))
            self.cli(image, 'export', '/large3', exported)
            self.assertEqual(exported.read_bytes(), final)
            self.cli(image, 'mkdir', '/node65', ok=False)
            self.assert_unchanged(image, original, set())
            host.write_bytes(final[:-39])
            self.cli(image, 'import', host, '/large3', '--replace')
            before, backups = image.read_bytes(), set(directory.glob('*.bak'))
            self.cli(image, 'mkdir', '/node65', ok=False)
            self.assert_unchanged(image, before, backups)
            host.write_bytes(final[:-40])
            self.cli(image, 'import', host, '/large3', '--replace')
            self.cli(image, 'mkdir', '/node65')
            _, _, nodes = volume.load(image.read_bytes())
            self.assertEqual(len(nodes), 65)
            self.assertEqual(sum(len(n['data']) for n in nodes.values()), volume.DATA_TOTAL_LIMIT - 40)
            self.assertEqual(nodes[volume.resolve(nodes, '/large0')]['data'], chunk)
            self.assertIn('Total file-data limit: 8,384,984 bytes', self.cli(image, 'info'))
            self.assertIn('Remaining file-data allowance: 0 bytes', self.cli(image, 'info'))

    def test_hundreds_files_host_exchange_and_full_accounting(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            image, host, exported = [directory / name for name in ('image', 'host', 'exported')]
            files = [(f'file{i}', bytes([i]) * (i + 1)) for i in range(250)]
            original = self.fixture(4, files=files)
            image.write_bytes(original)
            self.assertEqual(len(volume.load(original)[2]), 251)
            for i in range(250, 255):
                content = bytes(range(256)) * 257 + bytes([i])
                host.write_bytes(content)
                self.cli(image, 'import', host, f'/file{i}')
                self.cli(image, 'export', f'/file{i}', exported, '--replace')
                self.assertEqual(exported.read_bytes(), content)
            raw = image.read_bytes()
            self.assertEqual(len(volume.load(raw)[2]), 256)
            self.assertTrue(all(volume.decode(raw, slot) for slot in (0, 1)))
            self.assertIn('Node limit: 256', self.cli(image, 'info'))
            self.assertIn('Total file-data limit: 8,377,344 bytes', self.cli(image, 'info'))
            self.assertEqual(len(self.cli(image, 'ls', '/').splitlines()), 255)
            _, _, nodes = volume.load(raw)
            for i in range(250):
                self.assertEqual(nodes[volume.resolve(nodes, f'/file{i}')]['data'], files[i][1])
            backups = set(directory.glob('*.bak'))
            self.cli(image, 'import', host, '/overflow', ok=False)
            self.assert_unchanged(image, raw, backups)
            # Deleting an empty record returns its 40-byte metadata allowance.
            slot, generation, nodes = volume.load(raw)
            removed = nodes.pop(volume.resolve(nodes, '/file254'))
            volume.commit(image, raw, slot, generation, nodes)
            self.assertEqual(volume.DATA_LAYOUT.capacity(len(nodes)), 8377384)
            self.cli(image, 'import', host, '/replacement')
            self.cli(image, 'export', '/replacement', exported, '--replace')
            self.assertEqual(exported.read_bytes(), removed['data'])

    def nested_fixture(self, version, depth):
        """Construct a valid old tree with maximum-length names."""
        raw = self.fixture(version, files=[])
        layout = volume.disk_layout(raw)
        payload = bytearray()
        for ident in range(depth + 1):
            name = b'abcdefghijklmnopqrstuvw' if ident else b''
            payload += struct.pack('<HhBBHI24s', ident, ident - 1, 1, 0, 0, 0, name)
            if version >= 3:
                payload += struct.pack('<I', 1234)
        header = struct.pack('<6I', volume.MAGIC, version, depth + 1, len(payload),
                             volume.rolling(payload) if version == 1 else zlib.crc32(payload), 7)
        header += struct.pack('<I', 0 if version == 1 else zlib.crc32(header))
        start = layout.lbas[0] * 512
        raw[start:start + 512] = header.ljust(512, b'\0')
        raw[start + 512:start + 512 + len(payload)] = payload
        return raw

    def test_maximum_old_paths_and_host_import(self):
        path = '/abcdefghijklmnopqrstuvw' * 63
        self.assertEqual(len(path), 1512)
        for version in (1, 2, 3, 4):
            raw = self.nested_fixture(version, 63)
            nodes = volume.load(raw)[2]
            self.assertEqual(volume.resolve(nodes, path), 63)
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            image, host, exported = [directory / name for name in ('image', 'host', 'exported')]
            raw = self.nested_fixture(4, 62)
            image.write_bytes(raw)
            host.write_bytes(b'\x00binary\xff\r\n')
            self.cli(image, 'import', host, path)
            self.cli(image, 'export', path, exported)
            self.assertEqual(exported.read_bytes(), host.read_bytes())
            # A 63-component folder remains readable; another descendant does not fit.
            image.write_bytes(self.nested_fixture(4, 63))
            before, backups = image.read_bytes(), set(directory.glob('*.bak'))
            self.cli(image, 'mkdir', path + '/x', ok=False)
            self.cli(image, 'import', host, path + '/x', ok=False)
            self.assert_unchanged(image, before, backups)


if __name__ == '__main__':
    unittest.main()
