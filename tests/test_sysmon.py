"""Deterministic normal monitor and Terminal task lifecycle checks."""
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class SystemMonitorTests(unittest.TestCase):
    def native(self, name, extra=()):
        compiler = shutil.which('clang') or shutil.which('cc')
        self.assertIsNotNone(compiler, 'host C compiler required')
        with tempfile.TemporaryDirectory(prefix='baseos-monitor-') as directory:
            executable = pathlib.Path(directory) / name
            subprocess.run([compiler, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-I', str(ROOT / 'src'),
                            str(ROOT / 'tests' / (name + '.c')), *map(str, extra),
                            '-o', str(executable)], check=True)
            env = dict(os.environ)
            env['ASAN_OPTIONS'] = 'detect_leaks=0'
            env['UBSAN_OPTIONS'] = 'halt_on_error=1'
            subprocess.run([str(executable)], check=True, env=env)

    def test_monitor_layout_and_actions(self):
        self.native('sysmon_host')

    def test_terminal_task_metadata(self):
        self.native('term_task_info_host', [ROOT / 'tests/net_stub.c',
                                          ROOT / 'src/download.c', '-DDOWNLOAD_HOST_TEST'])


if __name__ == '__main__':
    unittest.main()
