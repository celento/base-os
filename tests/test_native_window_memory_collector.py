"""Host-only bounded page-admission collector checks; never starts QEMU."""
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools'))
import native_window_memory_input_test as collector
from native_window_input_test import ShellFont


class NativeWindowMemoryCollectorTests(unittest.TestCase):
    def test_public_snapshot_derives_page_admissions(self):
        default = collector.admission_plan(797-13, 269)
        large = collector.admission_plan(1021-13, 269)
        self.assertEqual((default['admitted'], default['remaining'], default['live_windows']), (2, 246, 3))
        self.assertEqual((large['admitted'], large['remaining'], large['live_windows']), (3, 201, 4))
        self.assertEqual(collector.admission_plan(538, 269)['remaining'], 0)
        with self.assertRaises(AssertionError): collector.admission_plan(268, 269)
        with self.assertRaises(AssertionError): collector.admission_plan(269*7, 269)
        with self.assertRaises(AssertionError): collector.admission_plan(784, 0)

    def test_new_fixture_header_commitment_and_unchanged_document_source(self):
        document = ROOT/'tests/native_window_document_app.c'
        before = collector.file_record(document)
        with tempfile.TemporaryDirectory(prefix='baseos-page-budget-host-') as temporary:
            target = Path(temporary)/collector.BUDGET_NAME
            collector.build_app(collector.BUDGET_SOURCE, target, format='bex2', window='native-v1',
                                workspace_bytes=collector.WORKSPACE_BYTES, stack_bytes=collector.STACK_BYTES)
            image = target.read_bytes(); header = collector._bex2_header(image)
            self.assertEqual(header[3], 1)
            self.assertEqual(header[10:14], (1048576, 16384, 1, 2))
            self.assertEqual(collector.declared_pages(image), 269)
        self.assertEqual(collector.file_record(document), before)

    def test_known_menu_diagnostic_bitmap(self):
        shell = ShellFont(); image = np.full((720, 1280, 3), 230, dtype=np.uint8)
        mask = shell.bitmap(collector.REFUSAL, 'ui'); x = 273
        image[9:27, x:x+mask.shape[1]][mask] = (216, 58, 58)
        self.assertTrue(collector.refusal_visible(image, shell))
        image[:] = 230
        self.assertFalse(collector.refusal_visible(image, shell))

    def test_public_identity_detects_owner_state_or_pool_change(self):
        state = dict(state=1, saved=0, verified=256, owned=269, free=246,
                     slot=3, phase=0, sleep=0, result=0)
        self.assertTrue(collector.is_budget(state, 269))
        for key in state:
            changed = dict(state); changed[key] += 1
            self.assertNotEqual(collector.stable_identity(state), collector.stable_identity(changed))
        changed = dict(state); changed['verified'] = 0
        self.assertFalse(collector.is_budget(changed, 269))

    def test_prepare_builds_only_new_named_fixture(self):
        with tempfile.TemporaryDirectory(prefix='baseos-memory-prepare-host-') as temporary:
            root = Path(temporary); parent = root/'parent'; parent.mkdir()
            observer = root/'window-document.bex'; observer.write_bytes(b'Immutable observer fixture')
            old = collector.file_record(observer); old['owned_pages'] = 13
            manifest = dict(build={}, apps={'window-document.bex': old}, source={}, build_directory=str(root/'held'),
                            build_info=dict(dirty=False, revision='frozen-runtime'))
            collector.save_json(parent/'manifest.json', manifest)
            calls = []
            def fake_build(source, target, **options):
                calls.append((source, target.name, options)); target.write_bytes(b'New fixture only')
            header = [0]*16; header[3] = 1; header[10:14] = [1048576, 16384, 1, 2]
            with mock.patch.object(collector, 'build_app', side_effect=fake_build), \
                 mock.patch.object(collector, '_bex2_header', return_value=tuple(header)), \
                 mock.patch.object(collector, 'declared_pages', return_value=269), \
                 mock.patch.object(collector, 'phase_run', side_effect=AssertionError('Guest execution is forbidden')):
                result = collector.prepare(parent, root/'prepared', profiles=())
            self.assertEqual(len(calls), 1)
            self.assertEqual(calls[0][0], collector.BUDGET_SOURCE)
            self.assertEqual(calls[0][1], 'page-budget.bex')
            self.assertEqual(observer.read_bytes(), b'Immutable observer fixture')
            self.assertTrue(result['preparation_only'])
            self.assertEqual(result['status'], 'PREPARED PAGE ADMISSION; NO GUEST RUN')
            self.assertFalse(result['runner_guest_qualified'])
            saved = json.loads((root/'prepared/manifest.json').read_text())
            self.assertEqual(saved['observer']['sha256'], old['sha256'])

if __name__ == '__main__':
    unittest.main()
