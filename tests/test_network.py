import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class NetworkTests(unittest.TestCase):
    def run_native(self, name):
        compiler = shutil.which('clang') or shutil.which('cc')
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as temp:
            exe = pathlib.Path(temp) / name
            subprocess.run([compiler, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra',
                            '-Werror', '-fsanitize=address,undefined', '-I', str(ROOT / 'src'),
                            str(ROOT / 'tests' / f'{name}.c'), str(ROOT / 'src/net_wire.c'),
                            '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_protocol_helpers(self):
        self.run_native('net_wire_host')

    def test_operation_lifecycle(self):
        self.run_native('net_state_host')


if __name__ == '__main__':
    unittest.main()
