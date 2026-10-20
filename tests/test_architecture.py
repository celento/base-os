"""Kernel/shell layering: source includes and, when built, object symbols.

Layers (docs/ARCHITECTURE.md): core = kernel + lib, then ui, apps and one
shell. A layer may use itself and the layers before it, never a later one, and
a shell never uses another shell. The core reaches the shell only through the
`baseos_shell` contract in kernel/shell.h.
"""
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / 'src'
RANK = {'kernel': 0, 'lib': 0, 'ui': 1, 'apps': 2, 'shell': 3}
CONTRACT = {'baseos_shell'}


def layer_of(path):
    """('kernel'|'lib'|'ui'|'apps'|'shell', shell name or None) for a src path."""
    parts = path.relative_to(SRC).parts
    return parts[0], parts[1] if parts[0] == 'shell' else None


def source_layers():
    """Object basename -> layer, from where each .c/.asm file lives."""
    layers = {}
    for path in SRC.rglob('*'):
        if path.suffix in ('.c', '.asm'):
            layers[path.stem] = layer_of(path)
    return layers


class IncludeLayeringTests(unittest.TestCase):
    def test_every_source_is_in_a_layer(self):
        for path in SRC.rglob('*'):
            if path.suffix in ('.c', '.h', '.asm', '.ld') and path.is_file():
                self.assertIn(path.relative_to(SRC).parts[0], RANK, path)

    def test_includes_point_down_the_stack(self):
        problems = []
        for path in sorted(SRC.rglob('*')):
            if path.suffix not in ('.c', '.h'):
                continue
            layer, shell = layer_of(path)
            for include in re.findall(r'^\s*#\s*include\s+"([^"]+)"', path.read_text(), re.M):
                top = include.split('/')[0]
                if top not in RANK:
                    continue  # same-folder, generated, sdk or third_party headers
                if RANK[top] > RANK[layer]:
                    problems.append(f'{path.relative_to(ROOT)} includes {include}')
                elif top == 'shell' and include.split('/')[1] != shell:
                    problems.append(f'{path.relative_to(ROOT)} includes another shell: {include}')
        self.assertEqual(problems, [])

    def test_kernel_starts_shell_only_through_the_contract(self):
        kernel = (SRC / 'kernel/kernel.c').read_text()
        for call in ('baseos_shell.prepare()', 'baseos_shell.start()', 'baseos_shell.turn()'):
            self.assertEqual(kernel.count(call), 1, call)
        shells = [path for path in (SRC / 'shell').iterdir() if path.is_dir()]
        self.assertGreaterEqual(len(shells), 2, 'desktop plus at least one alternative shell')
        for shell in shells:
            code = ''.join(p.read_text() for p in shell.glob('*.c'))
            self.assertEqual(len(re.findall(r'^const Shell baseos_shell\s*=', code, re.M)), 1, shell)
            self.assertNotIn('void kmain', code, shell)


class ObjectLayeringTests(unittest.TestCase):
    """Uses the normal build (make) when present; every symbol the core needs
    from outside itself must be the shell contract."""

    def setUp(self):
        self.build = ROOT / 'build'
        if not (self.build / 'kernel.elf').exists() or not shutil.which('nm'):
            self.skipTest('run make first; needs build/*.o and nm')

    def symbols(self, objects):
        defined, undefined = set(), {}
        for line in subprocess.check_output(['nm', '-A', *map(str, objects)], text=True).splitlines():
            location, _, rest = line.rpartition(':')
            fields = rest.split()
            if len(fields) == 2 and fields[0] == 'U':
                undefined.setdefault(fields[1], set()).add(Path(location).stem)
            elif len(fields) == 3 and fields[1] not in 'Uw':
                defined.add(fields[2])
        return defined, undefined

    def test_lower_layers_need_nothing_from_higher_ones(self):
        layers = source_layers()
        objects = sorted(self.build.glob('*.o'))
        known = [o for o in objects if o.stem in layers]
        self.assertGreater(len(known), 40)
        for limit in range(3):
            group = [o for o in known if RANK[layers[o.stem][0]] <= limit]
            defined, undefined = self.symbols(group)
            missing = {name: sorted(users) for name, users in undefined.items()
                       if name not in defined and name not in CONTRACT
                       and not name.startswith('__')}  # linker script symbols
            self.assertEqual(missing, {}, f'layer rank {limit} reaches upward')

    def test_kernel_links_with_a_stub_shell(self):
        cc = shutil.which('gcc')
        if not cc:
            self.skipTest('needs gcc with -m32')
        layers = source_layers()
        objects = [o for o in sorted(self.build.glob('*.o'))
                   if o.stem in layers and layers[o.stem][0] != 'shell']
        with tempfile.TemporaryDirectory(prefix='baseos-stub-shell-') as temporary:
            folder = Path(temporary)
            (folder / 'stub.c').write_text(
                '#include "kernel/shell.h"\n'
                'static void nothing(void){}\nstatic int idle(void){return 0;}\n'
                'const Shell baseos_shell={"stub",nothing,nothing,idle};\n')
            subprocess.run([cc, '-m32', '-ffreestanding', '-fno-pie', '-nostdlib', '-I', str(SRC),
                            '-c', str(folder / 'stub.c'), '-o', str(folder / 'stub.o')], check=True)
            subprocess.run(['ld', '-T', str(self.build / 'linker.ld'), '-nostdlib', '-m', 'elf_i386',
                            '-z', 'noexecstack', '-o', str(folder / 'kernel.elf'),
                            *map(str, objects), str(folder / 'stub.o')], check=True)


if __name__ == '__main__':
    unittest.main()
