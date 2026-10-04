"""Deterministic copied ProcessIO delivery against the real Terminal."""
from pathlib import Path
import unittest
import test_native_render

ROOT = Path(__file__).resolve().parents[1]


class ProcessBindingTests(unittest.TestCase):
    run_host = test_native_render.NativeRenderTests.run_host

    def test_exact_terminal_attachment(self):
        self.run_host('native_binding_host', extra=(str(ROOT / 'tests/net_stub.c'),
                      str(ROOT / 'src/download.c'), '-DDOWNLOAD_HOST_TEST'))


if __name__ == '__main__':
    unittest.main()
