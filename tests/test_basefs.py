"""BaseFS 2 core: format, mount, data round trips, free space, corruption
detection and a power cut at every sector of a commit, on a RAM disk."""
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import basefs  # noqa: E402


class BaseFsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('clang') or shutil.which('cc')
        if not compiler:
            raise unittest.SkipTest('host C compiler required')
        cls.directory = tempfile.TemporaryDirectory(prefix='baseos-basefs-')
        cls.executable = pathlib.Path(cls.directory.name) / 'basefs_host'
        subprocess.run([compiler, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-I', str(ROOT / 'src'),
                        str(ROOT / 'tests' / 'basefs_host.c'), '-o', str(cls.executable)],
                       check=True)
        cls.env = dict(os.environ, UBSAN_OPTIONS='halt_on_error=1')

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def run_tool(self, *args):
        return subprocess.run([str(self.executable), *map(str, args)], check=True, env=self.env,
                              capture_output=True, text=True).stdout

    def test_core(self):
        subprocess.run([str(self.executable)], check=True, env=self.env)

    def dump(self, image):
        lines = self.run_tool('dump', image).splitlines()
        return lines[0], [line.split(' ', 7) for line in lines[1:]]

    def test_kernel_reads_host_images(self):
        records = [dict(parent=-1, flags=basefs.DIRECTORY, identity=1, revision=0, modified=0,
                        data=b'', name=b''),
                   dict(parent=0, flags=basefs.DIRECTORY, identity=2, revision=1, modified=5,
                        data=b'', name=b'a folder name longer than the old twenty-three bytes'),
                   dict(parent=1, flags=0, identity=3, revision=2, modified=6,
                        data=bytes(range(256)) * 400, name=b'big.bin'),
                   dict(parent=0, flags=basefs.APP, identity=4, revision=3, modified=7,
                        data=b'', name=b'Paint'),
                   dict(parent=0, flags=0, identity=5, revision=4, modified=8, data=b'x',
                        name=b'one')]
        image = basefs.format_image(8192, [dict(r) for r in records])
        path = pathlib.Path(self.directory.name) / 'host.img'
        path.write_bytes(image)
        head, rows = self.dump(path)
        self.assertEqual(head, 'generation 1 next 6 5')
        self.assertEqual([(int(r[1]), int(r[5]), int(r[6], 16), r[7]) for r in rows],
                         [(r['parent'], len(r['data']), zlib.crc32(r['data']), r['name'].decode())
                          for r in records])
        # A second commit replaces one file and keeps the others' blocks.
        changed = [dict(r) for r in records]
        changed[4] = dict(changed[4], data=b'changed' * 1000, revision=9)
        image = basefs.update(image, changed)
        path.write_bytes(image)
        head, rows = self.dump(path)
        self.assertEqual(head, 'generation 2 next 6 10')
        self.assertEqual(int(rows[4][6], 16), zlib.crc32(b'changed' * 1000))
        self.assertEqual(int(rows[2][6], 16), zlib.crc32(bytes(range(256)) * 400))
        # Damage the newest checkpoint: the kernel falls back to generation 1.
        _, sup, _, _ = basefs.mount(image)
        start = sup['runs'][0][0]
        image[start * basefs.BLOCK + 20] ^= 1
        path.write_bytes(image)
        self.assertEqual(self.dump(path)[0], 'generation 1 next 6 5')

    def test_host_reads_kernel_images(self):
        path = pathlib.Path(self.directory.name) / 'kernel.img'
        self.run_tool('make', path, 16384)
        data = path.read_bytes()
        slot, sup, records, _ = basefs.mount(data)
        self.assertEqual((slot, sup['generation'], sup['next_revision']), (1, 2, 9))
        _, rows = self.dump(path)
        self.assertEqual([(r['parent'], len(r['data']), zlib.crc32(r['data']), r['name'].decode())
                          for r in records],
                         [(int(r[1]), int(r[5]), int(r[6], 16), r[7]) for r in rows])
        # The host can commit on top of a kernel image and the kernel reads it.
        records[3]['data'] = b'host edit'
        path.write_bytes(basefs.update(data, records))
        head, rows = self.dump(path)
        self.assertEqual(head.split()[1], '3')
        self.assertEqual(int(rows[3][6], 16), zlib.crc32(b'host edit'))


if __name__ == '__main__':
    unittest.main()
