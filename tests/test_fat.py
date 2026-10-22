"""FAT12/16/32 core (kernel/fat.c) against images made and checked by
dosfstools (mkfs.fat, fsck.fat) and mtools: reading their files, random
operation scripts compared with a model and with what mtools reads back, and
a power cut at many points of a script."""
import os
import pathlib
import random
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
TOOLS = ('mkfs.fat', 'fsck.fat', 'mcopy', 'mmd')
ENV = dict(os.environ, MTOOLS_SKIP_CHECK='1', LC_ALL='C.UTF-8', UBSAN_OPTIONS='halt_on_error=1')
# name, mkfs.fat arguments, size in KiB, largest file
KINDS = [('fat12', ['-F', '12'], 1440, 40000),
         ('fat16', ['-F', '16'], 16384, 400000),
         ('fat32', ['-F', '32', '-s', '1'], 66000, 600000)]
NAMES = ['README.TXT', 'notes.txt', 'Long File Name.html', 'game.exe', 'a.b.c.d', 'Mixed.Case',
         'Café Über.txt', 'data with spaces', 'x', '.hidden', 'SAVEGAME.001',
         'very long name ' + 'abcdefghij' * 20]


def pattern(seed, size, offset=0):
    return bytes((seed * 31 + i * 7 + (i >> 8)) & 255 for i in range(offset, offset + size))


def tool_missing():
    return [name for name in TOOLS if not shutil.which(name)]


class Model:
    """Names compare without ASCII case, as on FAT."""

    def __init__(self):
        self.root = {}

    def find(self, path):
        node = self.root
        for part in [p for p in path.split('/') if p]:
            if not isinstance(node, dict):
                return None
            key = next((k for k in node if k.lower() == part.lower()), None)
            if key is None:
                return None
            node = node[key]
        return node

    def parent(self, path):
        head, _, name = path.rpartition('/')
        return self.find(head), name

    def key(self, directory, name):
        return next((k for k in directory if k.lower() == name.lower()), None)


def fat_layout(image):
    """Boot sector fields for the checker."""
    b = image
    sectors_per_cluster, reserved, fats = b[13], struct.unpack_from('<H', b, 14)[0], b[16]
    root_entries = struct.unpack_from('<H', b, 17)[0]
    total = struct.unpack_from('<H', b, 19)[0] or struct.unpack_from('<I', b, 32)[0]
    fat_sectors = struct.unpack_from('<H', b, 22)[0] or struct.unpack_from('<I', b, 36)[0]
    root_sectors = (root_entries * 32 + 511) // 512
    data = reserved + fats * fat_sectors + root_sectors
    clusters = (total - data) // sectors_per_cluster
    bits = 12 if clusters < 4085 else 16 if clusters < 65525 else 32
    return dict(spc=sectors_per_cluster, reserved=reserved, fats=fats, root_entries=root_entries,
                fat_sectors=fat_sectors, data=data, clusters=clusters, bits=bits,
                root_cluster=struct.unpack_from('<I', b, 44)[0] if bits == 32 else 0)


def fat_entry(image, layout, n):
    base = layout['reserved'] * 512
    if layout['bits'] == 32:
        return struct.unpack_from('<I', image, base + n * 4)[0] & 0x0FFFFFFF
    if layout['bits'] == 16:
        return struct.unpack_from('<H', image, base + n * 2)[0]
    value = struct.unpack_from('<H', image, base + n + n // 2)[0]
    return value >> 4 if n & 1 else value & 0xFFF


def check_structure(test, image, twins=()):
    """No cluster belongs to two chains, no chain runs into a free or bad
    cluster, and every chain covers its file's size. Returns the files.
    twins: short names that may share a chain, as the old and new name of a
    file do while a rename is cut short."""
    layout = fat_layout(image)
    end = {12: 0xFF8, 16: 0xFFF8, 32: 0x0FFFFFF8}[layout['bits']]
    owner = {}
    chain_of = {}
    files = {}

    def chain(first, what):
        out, cluster = [], first
        while True:
            test.assertTrue(2 <= cluster < layout['clusters'] + 2, f'{what}: cluster {cluster} out of range')
            if cluster in owner and {what.rsplit('/', 1)[1], owner[cluster].rsplit('/', 1)[-1]} <= set(twins):
                return chain_of[owner[cluster]]
            test.assertNotIn(cluster, owner, f'{what}: cluster {cluster} shared with {owner.get(cluster)}')
            owner[cluster] = what
            out.append(cluster)
            value = fat_entry(image, layout, cluster)
            test.assertNotEqual(value, 0, f'{what}: chain runs into free cluster {cluster}')
            if value >= end:
                chain_of[what] = out
                return out
            cluster = value

    def entries(first, fixed):
        if fixed:
            start = (layout['reserved'] + layout['fats'] * layout['fat_sectors']) * 512
            raw = image[start:start + layout['root_entries'] * 32]
        else:
            raw = b''.join(image[(layout['data'] + (c - 2) * layout['spc']) * 512:
                                 (layout['data'] + (c - 1) * layout['spc']) * 512] for c in chain(first, 'dir'))
        for at in range(0, len(raw), 32):
            e = raw[at:at + 32]
            if e[0] == 0:
                return
            if e[0] == 0xE5 or e[11] & 0x3F == 0x0F or e[11] & 0x08 or e[:2] in (b'. ', b'..'):
                continue
            hi = struct.unpack_from('<H', e, 20)[0] if layout['bits'] == 32 else 0
            yield e[:11], e[11], hi << 16 | struct.unpack_from('<H', e, 26)[0], struct.unpack_from('<I', e, 28)[0]

    def walk(first, fixed, path):
        for name, attributes, cluster, size in entries(first, fixed):
            what = path + '/' + name.decode('latin-1').strip()
            if attributes & 0x10:
                test.assertTrue(cluster, f'{what}: directory without a cluster')
                walk(cluster, False, what)
            else:
                count = len(chain(cluster, what)) if cluster else 0
                bytes_per_cluster = layout['spc'] * 512
                test.assertGreaterEqual(count * bytes_per_cluster, size, f'{what}: chain shorter than size')
                files[what] = size

    if layout['bits'] == 32:
        walk(layout['root_cluster'], False, '')
    else:
        walk(0, True, '')
    return files


def flatten(node, prefix=''):
    """Path to size and CRC, so a mismatch reads as a short list."""
    out = {}
    for name, child in node.items():
        if isinstance(child, dict):
            out[prefix + name + '/'] = 'dir'
            out.update(flatten(child, prefix + name + '/'))
        else:
            out[prefix + name] = (len(child), zlib.crc32(child))
    return out


@unittest.skipIf(tool_missing(), 'needs dosfstools and mtools')
class FatTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('clang') or shutil.which('cc')
        if not compiler:
            raise unittest.SkipTest('host C compiler required')
        cls.directory = tempfile.TemporaryDirectory(prefix='baseos-fat-')
        cls.work = pathlib.Path(cls.directory.name)
        cls.executable = cls.work / 'fat_host'
        subprocess.run([compiler, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-I', str(ROOT / 'src'),
                        str(ROOT / 'tests' / 'fat_host.c'), '-o', str(cls.executable)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def tool(self, *args, stdin=None, check=True):
        result = subprocess.run([str(self.executable), *map(str, args)], input=stdin, env=ENV,
                                capture_output=True)
        if check and result.returncode:
            self.fail(f'fat_host {args}: {result.returncode}\n{result.stdout.decode()}{result.stderr.decode()}')
        return result

    def image(self, kind, label, offset=0):
        _, arguments, kib, _ = next(k for k in KINDS if k[0] == kind)
        path = self.work / f'{label}.img'
        path.unlink(missing_ok=True)
        if offset:
            body = self.work / f'{label}.part'
            body.unlink(missing_ok=True)
            subprocess.run(['mkfs.fat', '-C', *arguments, '-n', 'BASEOS', str(body), str(kib)],
                           check=True, capture_output=True)
            raw = bytearray(offset * 512) + body.read_bytes()
            raw[446:462] = struct.pack('<BBBBBBBBII', 0, 0, 0, 0, 0x0C, 0, 0, 0, offset, kib * 2)
            raw[510:512] = b'\x55\xaa'
            path.write_bytes(raw)
        else:
            subprocess.run(['mkfs.fat', '-C', *arguments, '-n', 'BASEOS', str(path), str(kib)],
                           check=True, capture_output=True)
        return path

    def fsck(self, path, offset=0):
        if offset:
            body = self.work / 'fsck.part'
            body.write_bytes(path.read_bytes()[offset * 512:])
            path = body
        result = subprocess.run(['fsck.fat', '-n', str(path)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def listing(self, path, directory):
        lines = self.tool('ls', path, directory).stdout.decode().splitlines()
        return {line.split(' ', 3)[3]: (line[0], int(line.split(' ')[1])) for line in lines}

    def test_self(self):
        self.assertEqual(self.tool('self').stdout.strip(), b'ok')

    def test_reads_mtools_files(self):
        for kind, _, _, largest in KINDS:
            with self.subTest(kind=kind):
                path = self.image(kind, 'read-' + kind)
                source = self.work / 'source'
                shutil.rmtree(source, ignore_errors=True)
                (source / 'Deep' / 'Er' / 'Est').mkdir(parents=True)
                files = {'Hello World.txt': pattern(1, 5), 'DOOM.EXE': pattern(2, largest),
                         'Café.txt': pattern(3, 513), 'Deep/Er/Est/inner file.dat': pattern(4, 4097),
                         'Deep/empty': b''}
                for name, data in files.items():
                    (source / name).write_bytes(data)
                for child in source.iterdir():
                    subprocess.run(['mcopy', '-s', '-i', str(path), str(child), '::'], check=True, env=ENV)
                info = self.tool('info', path).stdout.decode()
                self.assertIn(f'bits {kind[3:]} ', info)
                self.assertIn("label 'BASEOS'", info)
                self.assertEqual(self.listing(path, '/'), {'Hello World.txt': ('f', 5), 'DOOM.EXE': ('f', largest),
                                                         'Café.txt': ('f', 513), 'Deep': ('d', 0)})
                for name, data in files.items():
                    self.assertEqual(self.tool('cat', path, name).stdout, data, name)
                # Lookups ignore case and accept the short name.
                self.assertEqual(self.tool('cat', path, 'hello world.TXT').stdout, files['Hello World.txt'])
                self.assertEqual(self.tool('cat', path, 'DEEP/ER/EST/INNERF~1.DAT').stdout,
                                 files['Deep/Er/Est/inner file.dat'])
                self.assertNotEqual(self.tool('cat', path, 'nothing', check=False).returncode, 0)

    def test_partitioned_disk(self):
        path = self.image('fat32', 'partitioned', offset=2048)
        self.tool('script', path, stdin=b'put\tinside.bin\t9\t70000\t0\n')
        self.assertEqual(self.tool('cat', path, 'inside.bin').stdout, pattern(9, 70000))
        self.fsck(path, offset=2048)

    def random_script(self, rng, model, largest, count):
        lines = []
        directories = ['']

        def all_paths(node, prefix):
            for name, child in node.items():
                path = prefix + '/' + name if prefix else name
                yield path, child
                if isinstance(child, dict):
                    yield from all_paths(child, path)

        for _ in range(count):
            paths = list(all_paths(model.root, ''))
            directories = [''] + [p for p, c in paths if isinstance(c, dict)]
            files = [p for p, c in paths if not isinstance(c, dict)]
            choice = rng.random()
            if choice < 0.4 or not files:
                folder = rng.choice(directories)
                name = rng.choice(NAMES)
                path = folder + '/' + name if folder else name
                node = model.find(path)
                if isinstance(node, dict):
                    continue
                size = rng.choice([0, 1, 511, 512, 513, rng.randrange(1, largest)])
                old = node if node is not None else b''
                offset = rng.choice([0, len(old), rng.randrange(0, len(old) + 2000)])
                if offset + size > largest:
                    continue
                seed = rng.randrange(1, 1000)
                data = bytearray(old)
                # A write of nothing creates the file but does not extend it.
                if size and offset > len(data):
                    data.extend(bytes(offset - len(data)))
                data[offset:offset + size] = pattern(seed, size)
                parent, leaf = model.parent(path)
                parent[model.key(parent, leaf) or leaf] = bytes(data)
                lines.append(f'put\t{path}\t{seed}\t{size}\t{offset}')
            elif choice < 0.55:
                path = rng.choice(files)
                size = rng.randrange(0, largest)
                parent, leaf = model.parent(path)
                data = parent[leaf]
                parent[leaf] = data[:size] + bytes(max(0, size - len(data)))
                lines.append(f'trunc\t{path}\t{size}')
            elif choice < 0.7:
                folder = rng.choice(directories)
                name = rng.choice(NAMES)
                path = folder + '/' + name if folder else name
                if model.find(path) is not None or path.count('/') > 4:
                    continue
                parent, leaf = model.parent(path)
                parent[leaf] = {}
                lines.append(f'mkdir\t{path}')
            elif choice < 0.85:
                path = rng.choice([p for p, c in paths if not isinstance(c, dict) or not c])
                parent, leaf = model.parent(path)
                del parent[leaf]
                lines.append(f'rm\t{path}')
            else:
                source = rng.choice([p for p, _ in paths])
                folder = rng.choice(directories)
                name = rng.choice(NAMES)
                target = folder + '/' + name if folder else name
                if (folder + '/').lower().startswith(source.lower() + '/') or model.find(target) is not None:
                    continue
                parent, leaf = model.parent(source)
                node = parent.pop(leaf)
                destination, new_leaf = model.parent(target)
                destination[new_leaf] = node
                lines.append(f'mv\t{source}\t{target}')
            if rng.random() < 0.1:
                lines.append('sync')
        return '\n'.join(lines) + '\n'

    def compare(self, path, model):
        out = self.work / 'extract'
        shutil.rmtree(out, ignore_errors=True)
        out.mkdir()
        if model.root:
            subprocess.run(['mcopy', '-s', '-n', '-i', str(path), '::/*', str(out)], check=True, env=ENV,
                           capture_output=True)

        def tree(directory):
            return {child.name: tree(child) if child.is_dir() else child.read_bytes()
                    for child in directory.iterdir()}

        self.assertEqual(flatten(tree(out)), flatten(model.root))

        def ours(directory, node):
            names = self.listing(path, directory)
            self.assertEqual(set(names), set(node))
            for name, child in node.items():
                full = directory + '/' + name
                if isinstance(child, dict):
                    ours(full, child)
                else:
                    self.assertEqual(self.tool('cat', path, full).stdout, child, full)

        ours('', model.root)

    def test_random_scripts_match_mtools(self):
        for kind, _, _, largest in KINDS:
            for seed in range(3):
                with self.subTest(kind=kind, seed=seed):
                    path = self.image(kind, f'random-{kind}')
                    model = Model()
                    rng = random.Random(seed * 100 + len(kind))
                    for _ in range(3):
                        script = self.random_script(rng, model, largest, 40)
                        result = self.tool('script', path, stdin=script.encode())
                        self.assertNotIn(b'error', result.stdout)
                        self.fsck(path)
                        check_structure(self, path.read_bytes())
                    self.compare(path, model)

    def test_names_mtools_reads(self):
        path = self.image('fat16', 'names')
        script = ''.join(f'put\t{name}\t1\t3\t0\n' for name in NAMES)
        script += 'put\tlower.txt\t1\t1\t0\nput\tUPPER.TXT\t1\t1\t0\nput\tMixed.TXT\t1\t1\t0\n'
        script += ''.join(f'put\tsame prefix {i}.txt\t1\t1\t0\n' for i in range(12))
        # A basis that holds a tilde of its own still gets distinct tails.
        script += 'put\tab~cd long one.txt\t1\t1\t0\nput\tab~cd long two.txt\t1\t1\t0\n'
        self.tool('script', path, stdin=script.encode())
        self.fsck(path)
        listing = subprocess.run(['mdir', '-i', str(path), '::'], check=True, env=ENV, capture_output=True,
                                 text=True).stdout
        self.assertIn('lower    txt', listing)
        self.assertIn('SAMEPR~9 TXT', listing)
        self.assertIn('SAMEP~10 TXT', listing)
        self.assertIn('MIXED    TXT', listing)
        self.assertIn('AB~CDL~1 TXT', listing)
        self.assertIn('AB~CDL~2 TXT', listing)
        # A name already used as a file is written in place, whatever its case.
        self.tool('script', path, stdin=b'put\treadme.txt\t5\t2\t0\n')
        self.assertEqual(self.tool('cat', path, 'README.TXT').stdout, pattern(5, 2) + pattern(1, 3)[2:])
        errors = self.tool('script', path, stdin=b'mkdir\tREADME.TXT\nput\tbad:name\t1\t1\t0\n',
                           check=False).stdout
        self.assertEqual(errors.count(b'error'), 2)

    def test_stale_entries_past_the_end_stay_hidden(self):
        path = self.image('fat12', 'stale')
        self.tool('script', path, stdin=b'put\tKEEP.TXT\t1\t10\t0\nput\tA.TXT\t1\t10\t0\n'
                                       b'put\tGHOST.TXT\t2\t10\t0\n')
        raw = bytearray(path.read_bytes())
        raw[raw.index(b'A       TXT')] = 0      # an end marker, GHOST.TXT stale past it
        path.write_bytes(raw)
        self.assertEqual(set(self.listing(path, '/')), {'KEEP.TXT'})
        self.tool('script', path, stdin=b'put\tnew.txt\t3\t10\t0\n')
        self.assertEqual(set(self.listing(path, '/')), {'KEEP.TXT', 'new.txt'})
        listing = subprocess.run(['mdir', '-i', str(path), '::'], check=True, env=ENV, capture_output=True,
                                 text=True).stdout
        self.assertNotIn('GHOST', listing)

    def test_full_volume(self):
        path = self.image('fat12', 'full')
        script = ''.join(f'put\tfile{i}\t{i}\t300000\t0\n' for i in range(6))
        result = self.tool('script', path, stdin=script.encode(), check=False)
        self.assertIn(b'error -3', result.stdout)
        self.fsck(path)
        files = check_structure(self, path.read_bytes())
        # Files that did not fit were left empty, not half linked.
        self.assertIn(0, files.values())
        self.tool('script', path, stdin=b'rm\tfile0\nput\tafter\t3\t1000\t0\n')
        self.assertEqual(self.tool('cat', path, 'after').stdout, pattern(3, 1000))
        # The fixed FAT12 root holds 224 entries, one of them the label.
        root = self.image('fat12', 'root')
        script = ''.join(f'mkdir\tD{i}\n' for i in range(230))
        result = self.tool('script', root, stdin=script.encode(), check=False)
        self.assertEqual(result.stdout.count(b'error -3'), 230 - 223)
        self.fsck(root)

    def test_power_cut(self):
        for kind in ('fat16', 'fat32'):
            with self.subTest(kind=kind):
                base = self.image(kind, 'cut-base')
                self.tool('script', base, stdin=b'put\tkeep.bin\t3\t50000\t0\nmkdir\tDir\n'
                                                b'put\tDir/old name.txt\t4\t7000\t0\n')
                pristine = base.read_bytes()
                script = (b'put\tDir/new file with a long name.bin\t5\t90000\t0\n'
                          b'mv\tDir/old name.txt\tmoved here.txt\nput\tkeep.bin\t6\t30000\t40000\n'
                          b'trunc\tmoved here.txt\t10\nrm\tDir/new file with a long name.bin\n'
                          b'mkdir\tAnother Directory\nsync\n')
                trial = self.work / 'cut.img'
                trial.write_bytes(pristine)
                writes = int(self.tool('script', trial, stdin=script).stderr.decode().split('writes ')[1])
                for budget in sorted(set(range(0, writes, max(1, writes // 40))) | {writes - 1}):
                    trial.write_bytes(pristine)
                    self.tool('script', trial, budget, stdin=script)
                    files = check_structure(self, trial.read_bytes(), twins=('OLDNAM~1TXT', 'MOVEDH~1TXT'))
                    self.assertIn('/KEEP    BIN', files, budget)
                    # The file the script never touched in place reads back whole.
                    keep = self.tool('cat', trial, 'keep.bin').stdout
                    self.assertEqual(keep[:40000], pattern(3, 40000), budget)
                    repaired = self.work / 'repaired.img'
                    repaired.write_bytes(trial.read_bytes())
                    subprocess.run(['fsck.fat', '-a', '-w', str(repaired)], capture_output=True)
                    self.fsck(repaired)


if __name__ == '__main__':
    unittest.main()
