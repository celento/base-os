"""Deterministic incremental snapshot state and simulated transport checks."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class IncrementalFilesystemTests(unittest.TestCase):
    def run_host(self, source):
        compiler = shutil.which('clang') or shutil.which('cc')
        self.assertIsNotNone(compiler, 'host C compiler required')
        with tempfile.TemporaryDirectory(prefix='baseos-fs-sync-') as temp:
            binary = pathlib.Path(temp) / 'fs-sync'
            subprocess.run([compiler, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra',
                            '-Werror', '-fsanitize=address,undefined', '-I', str(ROOT / 'src'),
                            str(ROOT / 'tests' / source), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_bounded_jobs_lease_tickets_protocol_errors_and_capacity(self):
        self.run_host('fs_sync_host.c')

    def test_large_capacity_incremental_commit_and_arena_ownership(self):
        self.run_host('fs_sync_large_host.c')


if __name__ == '__main__':
    unittest.main()
