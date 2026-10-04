"""Independent zlib checks for filesystem CRCs, poll boundaries and saved bytes."""
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import volume


class FilesystemCrcTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix='baseos-crc-host-')
        cls.directory = pathlib.Path(cls.temporary.name)
        cls.binary = cls.directory / 'fs-crc'
        poll = cls.directory / 'poll.c'
        poll.write_text('unsigned crc_test_polls;\n'
                        'void fs_background_poll(void) { ++crc_test_polls; }\n')
        compiler = shutil.which('clang') or shutil.which('cc')
        if not compiler:
            raise RuntimeError('host C compiler required')
        subprocess.run([compiler, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-I', str(ROOT / 'src'),
                        str(ROOT / 'tests/fs_crc_host.c'), str(poll), '-o', str(cls.binary)],
                       check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def test_vectors_and_4096_byte_poll_boundaries(self):
        vectors = [b'', b'123456789'] + [bytes([i]) for i in range(256)]
        pattern = bytes((i * 37 + (i >> 16) + 19) & 255 for i in range(8388608))
        for length in (2, 7, 24, 28, 255, 256, 4095, 4096, 4097, 8191, 8192,
                       8193, 65535, 65536, 65537, 2097152, 8387584, 8388608):
            vectors.append(pattern[:length])
        vectors.extend((bytes(4096), b'\xff' * 4097, bytes(range(256)) * 257))
        request = b''.join(struct.pack('<I', len(data)) + data for data in vectors)
        result = subprocess.run([str(self.binary)], input=request, capture_output=True, check=True)
        lines = result.stdout.decode().splitlines()
        self.assertEqual(len(lines), len(vectors))
        for data, line in zip(vectors, lines):
            value, polls = line.split()
            self.assertEqual(int(value, 16), zlib.crc32(data), f'length {len(data)}')
            self.assertEqual(int(polls), len(data) // 4096, f'polls at length {len(data)}')

    def test_full_256_node_persisted_snapshots_match_zlib(self):
        path = self.directory / 'snapshot.img'
        subprocess.run([str(self.binary), '--snapshot', str(path)], check=True)
        raw = path.read_bytes()
        self.assertEqual(raw[:512], volume.data_marker())
        self.assertEqual(raw[-512:], bytes(512))
        layout = volume.disk_layout(raw)
        payloads = []
        for slot in (0, 1):
            start = layout.lbas[slot] * 512
            magic, version, count, length, checksum, generation, header_crc = struct.unpack_from('<7I', raw, start)
            self.assertEqual((magic, version, count, generation), (volume.MAGIC, 4, 256, slot + 1))
            self.assertEqual(length, 8387584)
            payload = raw[start + 512:start + 512 + length]
            self.assertEqual(header_crc, zlib.crc32(raw[start:start + 24]))
            self.assertEqual(checksum, zlib.crc32(payload))
            decoded = volume.decode(raw, slot)
            self.assertIsNotNone(decoded)
            self.assertEqual(len(decoded[1]), 256)
            self.assertEqual(sum(len(n['data']) for n in decoded[1].values()), 8377344)
            for ident in range(1, 5):
                data = decoded[1][ident]['data']
                expected = bytes((i * 37 + (i >> 16) + 19) & 255 for i in range(len(data)))
                self.assertEqual(data, expected)
            payloads.append(payload)
        self.assertEqual(payloads[0], payloads[1])


if __name__ == '__main__':
    unittest.main()
