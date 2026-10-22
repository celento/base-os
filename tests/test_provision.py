"""First-boot provisioning and the move to /Applications and /Scripts."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
# Stand-ins for the generated example headers; only their presence matters here.
HEADERS = {
    'audio_example.h': ['audio_example'],
    'native_example.h': ['native_example'],
    'sdk_examples.h': ['sdk_hello', 'sdk_notebook', 'sdk_counter', 'sdk_docstats',
                       'sdk_pointer', 'sdk_pointer_window'],
}


class ProvisionTests(unittest.TestCase):
    def test_layout_and_migration(self):
        compiler = shutil.which('clang') or shutil.which('cc')
        self.assertIsNotNone(compiler, 'host C compiler required')
        with tempfile.TemporaryDirectory(prefix='baseos-provision-') as temporary:
            temporary = Path(temporary)
            for header, names in HEADERS.items():
                (temporary / header).write_text(''.join(
                    f'static const unsigned char {name}[] = {{0x42,0x45,0x58,0x31}};\n' for name in names))
            output = temporary / 'provision'
            subprocess.run([compiler, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra',
                            '-Werror', '-fsanitize=address,undefined',
                            '-I', str(ROOT / 'src'), '-I', str(temporary),
                            str(ROOT / 'tests/provision_host.c'), '-o', str(output)], check=True)
            subprocess.run([str(output)], check=True,
                           env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0',
                                    UBSAN_OPTIONS='halt_on_error=1'))


if __name__ == '__main__':
    unittest.main()
