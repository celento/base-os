"""The test harness must reap its own emulator even after a timed-out check."""
import io
import pathlib
import subprocess
import sys
import unittest
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'tools'))
from qemu_session import DesktopSession


class Process:
    def __init__(self, exited=False, delayed=False):
        self.exited, self.delayed = exited, delayed
        self.stdin, self.stdout = io.BytesIO(), io.BytesIO()
        self.calls = []
    def poll(self):
        self.calls.append('poll')
        return 0 if self.exited else None
    def terminate(self): self.calls.append('terminate')
    def kill(self): self.calls.append('kill')
    def wait(self, timeout):
        self.calls.append('wait')
        if self.delayed and 'kill' not in self.calls:
            raise subprocess.TimeoutExpired('owned-qemu', timeout)
        self.exited = True
        return 0


class CleanupTests(unittest.TestCase):
    def check(self, process, expected):
        guest = object.__new__(DesktopSession)
        guest.process, guest.stderr = process, io.StringIO()
        guest.close()
        self.assertEqual(process.calls, expected)
        self.assertTrue(process.stdin.closed and process.stdout.closed and guest.stderr.closed)
    def test_normal_exit(self):
        self.check(Process(), ['poll', 'terminate', 'wait'])
    def test_reap_completed_process(self):
        self.check(Process(exited=True), ['poll', 'wait'])
    def test_bounded_force_cleanup(self):
        self.check(Process(delayed=True), ['poll', 'terminate', 'wait', 'kill', 'wait'])
