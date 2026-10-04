from contextlib import contextmanager
import hashlib
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import init_data
import volume


class VolumeHelpers:
    def fixture(self, version, *, files=None, generation=1):
        layout = volume.DATA_LAYOUT if version == 4 else volume.FLOPPY_LAYOUT
        image = bytearray(layout.sectors * 512)
        if version == 4:
            image[:512] = volume.data_marker()
        files = [('old.txt', b'old')] if files is None else files
        payload = bytearray()
        for ident, (name, content) in enumerate([('', b'')] + files):
            payload += struct.pack('<HhBBHI24s', ident, 0 if ident else -1,
                                   int(ident == 0), 0, 0, len(content), name.encode('ascii'))
            if version >= 3:
                payload += struct.pack('<I', 1234 if ident else 0)
            payload += content
        header = struct.pack('<6I', volume.MAGIC, version, 1 + len(files), len(payload),
                             volume.rolling(payload) if version == 1 else zlib.crc32(payload), generation)
        header += struct.pack('<I', 0 if version == 1 else zlib.crc32(header))
        start = layout.lbas[0] * 512
        image[start:start + 28] = header
        image[start + 512:start + 512 + len(payload)] = payload
        return image

    def cli_result(self, image, *args, ok=True, tool='volume.py'):
        result = subprocess.run([sys.executable, str(ROOT / 'tools' / tool), str(image), *map(str, args)],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode == 0, ok, result.stderr)
        return result

    def cli(self, image, *args, ok=True):
        return self.cli_result(image, *args, ok=ok).stdout

    def assert_unchanged(self, image, before, backups):
        self.assertEqual(image.read_bytes(), before)
        self.assertEqual(set(image.parent.glob('*.bak')), backups)

    @contextmanager
    def lock_elsewhere(self, image):
        child = subprocess.Popen([
            sys.executable, '-c',
            'import fcntl,sys; f=open(sys.argv[1],"r+b"); '
            'fcntl.lockf(f,fcntl.LOCK_EX); print("ready",flush=True); sys.stdin.read()', str(image)
        ], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        try:
            self.assertEqual(child.stdout.readline().strip(), b'ready')
            yield
        finally:
            child.communicate(timeout=5)


class VolumeTests(VolumeHelpers, unittest.TestCase):
    def test_versions_import_export_backups(self):
        for version in (1, 2, 3):
            with self.subTest(version=version), tempfile.TemporaryDirectory() as temp:
                d = pathlib.Path(temp)
                image = d / 'disk'
                original = self.fixture(version)
                image.write_bytes(original)
                self.assertEqual(volume.load(original)[2][1]['data'], b'old')
                self.cli(image, 'mkdir', '/Test')
                host = d / 'binary'
                content = bytes(range(256)) * 63 + b'\0last'
                host.write_bytes(content)
                self.cli(image, 'import', host, '/Test/binary')
                output = d / 'export'
                self.cli(image, 'export', '/Test/binary', output)
                self.assertEqual(output.read_bytes(), content)
                self.cli(image, 'export', '/Test/binary', output, ok=False)
                before = image.read_bytes()
                self.cli(image, 'import', host, '/old.txt', ok=False)
                self.assertEqual(image.read_bytes(), before)
                self.cli(image, 'import', host, '/old.txt', '--replace')
                self.assertEqual(volume.load(image.read_bytes())[2][1]['data'], content)
                self.assertTrue(any(p.read_bytes() == original for p in d.glob('*.bak')))
                host.write_bytes(bytes(16383))
                self.cli(image, 'import', host, '/boundary')
                host.write_bytes(bytes(16384))
                before, backups = image.read_bytes(), set(d.glob('*.bak'))
                self.cli(image, 'import', host, '/too-big', ok=False)
                self.assert_unchanged(image, before, backups)
                self.assertIn('16,383 bytes', self.cli(image, 'info'))

    def test_legacy_small_floppy_read_only_until_upgrade(self):
        with tempfile.TemporaryDirectory() as temp:
            image = pathlib.Path(temp) / 'disk'
            before = self.fixture(1)[:2880 * 512]
            image.write_bytes(before)
            self.assertEqual(volume.load(before)[2][1]['data'], b'old')
            self.assertIn('old.txt', self.cli(image, 'ls'))
            result = self.cli_result(image, 'mkdir', '/new', ok=False)
            self.assertIn('upgrade', result.stderr)
            self.assert_unchanged(image, before, set())

    def test_data_large_import_export_preserves_snapshot_and_backup(self):
        with tempfile.TemporaryDirectory() as temp:
            d = pathlib.Path(temp)
            image, host, output = d / 'disk', d / 'audio.wav', d / 'export.wav'
            original = self.fixture(4)
            image.write_bytes(original)
            content = b'RIFF' + bytes(range(256)) * 1024 + b'\0WAVE'
            host.write_bytes(content)
            self.cli(image, 'import', host, '/sample.wav')
            updated = image.read_bytes()
            active, generation, nodes = volume.load(updated)
            self.assertEqual((active, generation), (1, 2))
            self.assertEqual(nodes[volume.resolve(nodes, '/sample.wav')]['data'], content)
            self.cli(image, 'export', '/sample.wav', output)
            self.assertEqual(output.read_bytes(), content)
            first, second = (lba * 512 for lba in volume.DATA_LAYOUT.lbas)
            self.assertEqual(updated[:second], original[:second])
            self.assertEqual(updated[-512:], original[-512:])
            self.assertEqual(struct.unpack_from('<I', updated, second + 4)[0], 4)
            self.assertEqual(volume.decode(updated, 0), volume.decode(original, 0))
            self.assertEqual(next(d.glob('*.bak')).read_bytes(), original)
            self.cli(image, 'mkdir', '/Pictures')
            after = image.read_bytes()
            self.assertEqual(after[second:second + volume.DATA_SLOT_SECTORS * 512],
                             updated[second:second + volume.DATA_SLOT_SECTORS * 512])
            self.assertEqual(after[:512], volume.data_marker())
            self.assertEqual(volume.load(after)[0], 0)
            info = self.cli(image, 'info')
            self.assertIn('Type: data (16,777,216 bytes)', info)
            self.assertIn('2,097,152 bytes', info)
            self.assertIn('8,385,024 bytes', info)

    def test_data_two_mib_boundary_and_oversize_no_writes(self):
        with tempfile.TemporaryDirectory() as temp:
            d = pathlib.Path(temp)
            image, host, output = d / 'disk', d / 'host', d / 'export'
            image.write_bytes(self.fixture(4))
            content = bytes(range(256)) * (volume.DATA_FILE_LIMIT // 256)
            host.write_bytes(content)
            self.cli(image, 'import', host, '/boundary.bin')
            self.cli(image, 'export', '/boundary.bin', output)
            self.assertEqual(output.read_bytes(), content)
            host.write_bytes(content + b'!')
            before, backups = image.read_bytes(), set(d.glob('*.bak'))
            for destination, flags in (('/oversize.bin', []), ('/boundary.bin', ['--replace'])):
                result = self.cli_result(image, 'import', host, destination, *flags, ok=False)
                self.assertIn('File exceeds 2,097,152 bytes', result.stderr)
                self.assert_unchanged(image, before, backups)

    def test_total_capacity_boundary_and_replacement(self):
        with tempfile.TemporaryDirectory() as temp:
            d = pathlib.Path(temp)
            image, host = d / 'disk', d / 'host'
            chunk = b'a' * volume.DATA_FILE_LIMIT
            image.write_bytes(self.fixture(4, files=[(f'{i}.bin', chunk) for i in range(3)]))
            remaining = volume.DATA_TOTAL_LIMIT - 3 * len(chunk)
            host.write_bytes(b'b' * remaining)
            self.cli(image, 'import', host, '/last.bin')
            data = image.read_bytes()
            self.assertEqual(sum(len(n['data']) for n in volume.load(data)[2].values()), volume.DATA_TOTAL_LIMIT)
            self.assertIn('Remaining file-data allowance: 0 bytes', self.cli(image, 'info'))
            host.write_bytes(b'x')
            backups = set(d.glob('*.bak'))
            result = self.cli_result(image, 'import', host, '/extra', ok=False)
            self.assertIn('Volume full', result.stderr)
            self.assert_unchanged(image, data, backups)
            host.write_bytes(b'b' * (remaining + 1))
            self.cli(image, 'import', host, '/last.bin', '--replace', ok=False)
            self.assert_unchanged(image, data, backups)
            host.write_bytes(b'free space')
            self.cli(image, 'import', host, '/last.bin', '--replace')
            self.assertLess(sum(len(n['data']) for n in volume.load(image.read_bytes())[2].values()), volume.DATA_TOTAL_LIMIT)

    def test_node_limit_no_writes(self):
        for version, limit in ((3, volume.LEGACY_NODES), (4, volume.MAX_NODES)):
            with self.subTest(version=version), tempfile.TemporaryDirectory() as temp:
                image = pathlib.Path(temp) / 'disk'
                image.write_bytes(self.fixture(version, files=[(f'{i}.txt', b'') for i in range(limit - 2)]))
                self.cli(image, 'mkdir', '/last')
                before, backups = image.read_bytes(), set(image.parent.glob('*.bak'))
                self.assertEqual(len(volume.load(before)[2]), limit)
                self.assertIn(f'Node limit: {limit}', self.cli(image, 'info'))
                self.cli(image, 'mkdir', '/one-too-many', ok=False)
                self.assert_unchanged(image, before, backups)

    def test_corrupt_latest_falls_back(self):
        for version in (3, 4):
            with self.subTest(version=version), tempfile.TemporaryDirectory() as temp:
                image = pathlib.Path(temp) / 'disk'
                image.write_bytes(self.fixture(version))
                self.cli(image, 'mkdir', '/new')
                data = bytearray(image.read_bytes())
                layout = volume.disk_layout(data)
                data[layout.lbas[1] * 512 + 520] ^= 1
                self.assertEqual(volume.load(data)[0], 0)
                image.write_bytes(data)
                self.cli(image, 'mkdir', '/recovered')
                self.assertEqual(volume.load(image.read_bytes())[2][1]['data'], b'old')
                start = layout.lbas[0] * 512
                self.assertEqual(image.read_bytes()[start:start + layout.slot_sectors * 512],
                                 data[start:start + layout.slot_sectors * 512])

    def test_generation_wraparound(self):
        with tempfile.TemporaryDirectory() as temp:
            image = pathlib.Path(temp) / 'disk'
            image.write_bytes(self.fixture(4, generation=0xffffffff))
            self.cli(image, 'mkdir', '/new')
            self.assertEqual(volume.load(image.read_bytes())[:2], (1, 0))

    def test_wrong_snapshot_version_rejected(self):
        for source_version, changed_version in ((3, 4), (4, 3)):
            with self.subTest(source=source_version):
                data = self.fixture(source_version)
                start = volume.disk_layout(data).lbas[0] * 512
                struct.pack_into('<I', data, start + 4, changed_version)
                struct.pack_into('<I', data, start + 24, zlib.crc32(data[start:start + 24]))
                self.assertIsNone(volume.decode(data, 0))
                with self.assertRaises(ValueError):
                    volume.load(data)

    def test_image_lock(self):
        for version in (3, 4):
            with self.subTest(version=version), tempfile.TemporaryDirectory() as temp:
                image = pathlib.Path(temp) / 'disk'
                before = self.fixture(version)
                image.write_bytes(before)
                with self.lock_elsewhere(image):
                    self.cli(image, 'mkdir', '/blocked', ok=False)
                    self.cli(image, 'info', ok=False)
                    slot, generation, nodes = volume.load(before)
                    with self.assertRaises(BlockingIOError):
                        volume.commit(image, before, slot, generation, nodes)
                    if version == 4:
                        self.cli_result(image, tool='init_data.py', ok=False)
                    self.assert_unchanged(image, before, set())

    def test_stale_direct_commit_no_writes(self):
        with tempfile.TemporaryDirectory() as temp:
            image = pathlib.Path(temp) / 'disk'
            old = self.fixture(4)
            image.write_bytes(old)
            slot, generation, nodes = volume.load(old)
            self.cli(image, 'mkdir', '/new')
            before, backups = image.read_bytes(), set(image.parent.glob('*.bak'))
            with self.assertRaisesRegex(ValueError, 'changed'):
                volume.commit(image, old, slot, generation, nodes)
            self.assert_unchanged(image, before, backups)

    def test_export_cannot_overwrite_image_or_hardlink(self):
        with tempfile.TemporaryDirectory() as temp:
            image = pathlib.Path(temp) / 'disk'
            before = self.fixture(4)
            image.write_bytes(before)
            alias = image.with_name('alias')
            alias.hardlink_to(image)
            for target in (image, alias):
                self.cli(image, 'export', '/old.txt', target, '--replace', ok=False)
                self.assert_unchanged(image, before, set())

    def test_mismatching_existing_backup_refuses_update(self):
        with tempfile.TemporaryDirectory() as temp:
            image = pathlib.Path(temp) / 'disk'
            before = self.fixture(4)
            image.write_bytes(before)
            backup = image.with_name(image.name + '.' + hashlib.sha256(before).hexdigest()[:16] + '.bak')
            backup.write_bytes(b'keep this unrelated backup')
            self.cli(image, 'mkdir', '/new', ok=False)
            self.assert_unchanged(image, before, {backup})
            self.assertEqual(backup.read_bytes(), b'keep this unrelated backup')


class DataInitializationTests(VolumeHelpers, unittest.TestCase):
    def test_marker_create_and_byte_preserving_rerun(self):
        with tempfile.TemporaryDirectory() as temp:
            image = pathlib.Path(temp) / 'nested' / 'data.img'
            self.assertTrue(init_data.initialize(image))
            before, inode = image.read_bytes(), image.stat().st_ino
            self.assertEqual(len(before), 32768 * 512)
            self.assertEqual(struct.unpack_from('<7I', before),
                             (0x44534f42, 1, 32768, 16383, 1, 16384, zlib.crc32(before[:24])))
            self.assertEqual(before[28:], bytes(len(before) - 28))
            self.assertFalse(init_data.initialize(image))
            self.assert_unchanged(image, before, set())
            self.assertEqual(image.stat().st_ino, inode)
            image.write_bytes(self.fixture(4))
            before = image.read_bytes()
            result = self.cli_result(image, tool='init_data.py')
            self.assertIn('left unchanged', result.stdout)
            self.assert_unchanged(image, before, set())

    def test_blank_requires_boot_before_mutation(self):
        with tempfile.TemporaryDirectory() as temp:
            d = pathlib.Path(temp)
            image, host = d / 'disk', d / 'host'
            init_data.initialize(image)
            before = image.read_bytes()
            host.write_bytes(b'hello')
            self.assertIn('boot BaseOS once', self.cli(image, 'info'))
            for args in (('mkdir', '/new'), ('import', host, '/new'), ('ls',)):
                result = self.cli_result(image, *args, ok=False)
                self.assertIn('boot BaseOS once', result.stderr)
                self.assert_unchanged(image, before, set())
            # Recognized marker plus no valid snapshot must never be reformatted.
            data = bytearray(before)
            data[512] = 1
            image.write_bytes(data)
            self.cli(image, 'info', ok=False)
            self.cli(image, 'mkdir', '/new', ok=False)
            self.assert_unchanged(image, data, set())
            self.assertFalse(init_data.initialize(image))
            self.assert_unchanged(image, data, set())

    def test_reject_invalid_markers_and_unknown_images_without_writes(self):
        valid = self.fixture(4)
        cases = [('unknown size', b'valuable existing disk'), ('legacy floppy', self.fixture(3)),
                 ('unmarked data image', bytes(len(valid))), ('truncated data image', valid[:-512])]
        for label, offset in (('bad magic', 0), ('wrong marker version', 4), ('wrong disk sectors', 8),
                              ('wrong slot sectors', 12), ('wrong first lba', 16), ('wrong second lba', 20),
                              ('bad checksum', 24), ('nonzero reserved marker bytes', 28)):
            data = bytearray(valid)
            data[offset] ^= 1
            if offset < 24:
                struct.pack_into('<I', data, 24, zlib.crc32(data[:24]))
            cases.append((label, data))
        for label, before in cases:
            with self.subTest(label=label), tempfile.TemporaryDirectory() as temp:
                image = pathlib.Path(temp) / 'disk'
                image.write_bytes(before)
                self.cli_result(image, tool='init_data.py', ok=False)
                self.assert_unchanged(image, before, set())
                if label != 'legacy floppy':
                    self.cli(image, 'mkdir', '/new', ok=False)
                    self.assert_unchanged(image, before, set())


if __name__ == '__main__':
    unittest.main()
