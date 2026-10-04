"""Validate the read-only layout helper against ordinary compiler output."""
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from elf_debug import DebugInfo


class DebugLayoutTests(unittest.TestCase):
    def test_source_unit_resolves_repeated_static_names(self):
        compiler = shutil.which('gcc') or shutil.which('cc')
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            (root / 'first.c').write_text('typedef struct { char tag; int value; } Sample;\n'
                'static Sample state = {1, 2}; int other(void);\n'
                'int main(void) { return state.value + other(); }\n')
            (root / 'second.c').write_text('static struct { int total; char ready; } state = {3, 1};\n'
                'int other(void) { return state.total + state.ready; }\n')
            binary = root / 'ordinary-layout'
            subprocess.run([compiler, '-g', '-O0', str(root / 'first.c'), str(root / 'second.c'),
                            '-o', str(binary)], check=True)
            info = DebugInfo(binary)
            size, fields = info.structure('Sample', 'first.c')
            self.assertEqual((size, fields), (8, {'tag': 0, 'value': 4}))
            first, layout = info.variable('state', 'first.c')
            second, _ = info.variable('state', 'second.c')
            self.assertNotEqual(first, second)
            self.assertEqual(layout, (size, fields))
            with self.assertRaises(ValueError):
                info.variable('state')


if __name__ == '__main__':
    unittest.main()
