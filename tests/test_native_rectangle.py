"""Ordinary exact-pixel and lifecycle coverage for the optional rectangle backend."""
from pathlib import Path
import unittest
import test_native_render

ROOT = Path(__file__).resolve().parents[1]


class NativeRectangleTests(unittest.TestCase):
    run_host = test_native_render.NativeRenderTests.run_host

    def test_scalar_equivalence_and_lifecycle(self):
        self.run_host('native_rectangle_host', extra=(str(ROOT / 'tests/net_stub.c'),
                      str(ROOT / 'src/download.c'), '-DDOWNLOAD_HOST_TEST'))


if __name__ == '__main__':
    unittest.main()
