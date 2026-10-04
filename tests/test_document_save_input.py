"""Host-only safety/format tests for the ordinary document guest collector."""
import pathlib
import os
import struct
import sys
import tempfile
import time
import types
import unittest
from unittest import mock
from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from document_save_input_test import (ENTRY_BYTES, MANIFEST_LIMIT, MANIFEST_MAGIC,
    Session, StoppedDisk, expected_outputs, fixture, fnv, manifest_bytes, native_sheet, native_writer,
    pending_generations, snapshot_jobs, verify_payloads)
from init_data import initialize
from volume import data_layout, encode_snapshot, load, resolve


def writer(text):
    return struct.pack('<4sHHII', b'BWR1', 1, 0, len(text), 0) + text + bytes(2 * (len(text) + 1))


def seed(path):
    initialize(path)
    nodes = {ident: dict(parent=parent, name=name, directory=1, app=0, data=b'', modified=0)
             for ident, parent, name in ((0, -1, ''), (1, 0, 'Documents'), (2, 0, 'Programs'))}
    header, payload = encode_snapshot(nodes, data_layout(), 1)
    raw = bytearray(path.read_bytes()); raw[512:1024] = header.ljust(512, b'\0')
    raw[1024:1024 + len(payload)] = payload; path.write_bytes(raw)


class DocumentCollectorTest(unittest.TestCase):
    def test_serial_generation_pairing_wrap_and_errors(self):
        log = ('FS snapshot begin tick=FFFFFFFA generation=00000007\n'
               'FS snapshot end tick=00000004 generation=00000007 result=durable\n'
               'FS snapshot begin tick=00000006 generation=00000008\n')
        self.assertEqual(pending_generations(log), [8])
        self.assertAlmostEqual(snapshot_jobs(log)[0]['duration_seconds'], 10 / 70)
        with self.assertRaises(AssertionError):
            snapshot_jobs('FS snapshot end tick=00000004 generation=00000007 result=durable\n')
        with self.assertRaises(AssertionError):
            snapshot_jobs(log + 'FS snapshot begin tick=00000008 generation=00000008\n')

    def test_serial_partial_terminal_record_stays_pending(self):
        begin = 'FS snapshot begin tick=00000001 generation=00000002\n'
        end = 'FS snapshot end tick=00000007 generation=00000002 result=durable\n'
        for length in range(len(end)):
            self.assertEqual(pending_generations(begin + end[:length]), [2])
        self.assertEqual(pending_generations(begin + end), [])
        self.assertEqual(snapshot_jobs(begin + end)[0]['result'], 'durable')

    def test_native_writer_rejects_truncation_and_extra_bytes(self):
        self.assertEqual(native_writer(writer(b'ALPHAZ')),
                         dict(text=b'ALPHAZ', style=bytes(7), paragraph=bytes(7)))
        for data in (b'', writer(b'A')[:-1], writer(b'A') + b'X'):
            with self.assertRaises(AssertionError):
                native_writer(data)

    def test_native_sheet_sources_and_bounds(self):
        header = struct.pack('<4sHHHHI', b'BSH1', 1, 0, 128, 26, 1)
        data = header + struct.pack('<HBB', 0, 2, 6) + b'27.125'
        self.assertEqual(native_sheet(data), {(0, 0): (2, '27.125')})
        for bad in (b'', data[:-1], data + b'X', header + struct.pack('<HBB', 3328, 2, 0)):
            with self.assertRaises(AssertionError):
                native_sheet(bad)

    def test_manifest_is_bounded_and_keeps_zero_length_file(self):
        files = [('/Documents/async.bwr', writer(b'ALPHA')), ('/Documents/empty.csv', b'')]
        data = manifest_bytes(files)
        self.assertEqual(struct.unpack_from('<4I', data), (MANIFEST_MAGIC, 1, 2, 0))
        self.assertEqual(len(data), 16 + 2 * ENTRY_BYTES)
        for index, (path, contents) in enumerate(files):
            raw_path, length, checksum = struct.unpack_from('<128sII', data, 16 + index * ENTRY_BYTES)
            self.assertEqual((raw_path.rstrip(b'\0').decode(), length, checksum),
                             (path, len(contents), fnv(contents)))
        self.assertLessEqual(len(data), MANIFEST_LIMIT)
        with self.assertRaises(AssertionError):
            manifest_bytes([])
        with self.assertRaises(AssertionError):
            manifest_bytes(files * 16)
        with self.assertRaises(AssertionError):
            manifest_bytes([('relative', b'A')])

    def test_memory_debugger_and_running_disk_routes_are_rejected(self):
        session = Session.__new__(Session)
        with self.assertRaises(AssertionError):
            session.memory(0, 1)
        for command in ('pmemsave', 'human-monitor-command', 'x-exit-preconfig'):
            with self.assertRaises(AssertionError):
                session.command(command)
        disk = StoppedDisk('/does-not-exist'); disk.active = True
        with self.assertRaisesRegex(AssertionError, 'Running-image'):
            disk.read()
        self.assertFalse(hasattr(disk, 'install_manifest'))

    def test_input_clock_excludes_ocr_processing_and_requires_same_pending_generation(self):
        session = Session.__new__(Session)
        session.keep_awake = lambda: None
        frame = dict(ocr='ALPHAZ Saving', dumped=12.125, pending_generations=[7])
        session.frame = lambda name: dict(frame)
        result = session.visible('edit', 'ALPHAZ', generation=7, since=12.0)
        self.assertEqual(result['input_to_visible_upper_bound_ms'], 125)
        with self.assertRaises(AssertionError):
            session.visible('edit', 'ALPHAZ', generation=8, since=12.0)

    def test_admission_timing_cannot_attribute_an_old_boundary(self):
        session = Session.__new__(Session)
        session.events, session.admissions, session.begin_observed = [], {}, {7: 12.08}
        session.record_admission(7, 12.0)
        self.assertAlmostEqual(session.admissions[7]['submit_to_serial_begin_upper_bound_ms'], 80)
        with self.assertRaises(AssertionError):
            session.record_admission(7, 13.0)

    def test_accepted_name_frame_reuses_same_pending_pixels_without_another_dump(self):
        session = Session.__new__(Session)
        session.last_accepted_name_frame = dict(ocr='ALPHA Saving | New edits stay private.',
                                                pending_generations=[7], dumped=12.125)
        session.complete_ocr = lambda event, texts: None
        session.frame = lambda *args: self.fail('Acceptance must reuse the already captured frame')
        event = session.accepted_name_frame('ALPHA', 'Saving', generation=7, absent=('Save document as',))
        self.assertEqual(event['dumped'], 12.125)
        with self.assertRaises(AssertionError):
            session.accepted_name_frame('ALPHA', generation=8)
        with self.assertRaises(AssertionError):
            session.accepted_name_frame('ALPHAZ', generation=7)

    def test_crop_fallback_uses_same_pixels_pending_generation_and_timestamp(self):
        with tempfile.TemporaryDirectory() as temporary:
            session = Session.__new__(Session)
            session.directory = pathlib.Path(temporary)
            screenshot = session.directory / 'guard.png'
            Image.new('RGB', (1280, 720), 'white').save(screenshot)
            event = dict(ocr='ALPHAZC', screenshot=str(screenshot), dumped=12.125,
                         pending_generations=[7])
            answer = types.SimpleNamespace(stdout='Saving changes...\nCancel keeps this document open.\n')
            with mock.patch('document_save_input_test.subprocess.run', return_value=answer) as command:
                session.complete_ocr(event, ('Saving changes', 'Cancel keeps'))
            self.assertEqual(command.call_count, 1)
            self.assertEqual(event['dumped'], 12.125)
            self.assertEqual(event['pending_generations'], [7])
            self.assertEqual(event['ocr_full_frame'], 'ALPHAZC')
            self.assertEqual(event['ocr_crops'][0]['bounds'], [320, 120, 960, 500])
            self.assertEqual(event['ocr_crops'][0]['psm'], 6)
            session.complete_ocr(event, ('Saving changes', 'Cancel keeps'))
            self.assertEqual(len(event['ocr_crops']), 1)

    def test_missing_dirty_status_selects_footer_crop_first(self):
        session = Session.__new__(Session)
        event = dict(ocr='ALPHAZ')
        regions = []
        def crop(evidence, region):
            regions.append(region)
            evidence['ocr'] += '\nUnsaved | Recovered document draft.'
        session.crop_ocr = crop
        session.complete_ocr(event, ('ALPHAZ', 'Unsaved'))
        self.assertEqual(regions, ['footer'])

    def test_floppy_does_not_wait_for_async_markers(self):
        session = Session.__new__(Session)
        session.asynchronous = False
        session.idle = lambda: None
        session.serial = lambda: ''
        session.new_job = lambda prior: self.fail('Floppy cannot require async serial markers')
        self.assertIsNone(session.submit(lambda: 1.0))
        self.assertTrue(session.durable(None)['synchronous'])

    def test_keep_awake_finishes_shift_release_before_next_action(self):
        session = Session.__new__(Session)
        session.last_key = 0
        calls = []
        session.key = lambda key, delay: calls.append((key, delay))
        with mock.patch('document_save_input_test.time.monotonic', return_value=20):
            session.keep_awake()
        self.assertEqual(calls, [('shift', .08)])
        self.assertGreater(calls[0][1], .035)

    def test_qmp_partial_line_is_deadline_bounded_and_buffered(self):
        session = Session.__new__(Session)
        session.qmp_buffer = b''
        read_fd, write_fd = os.pipe()
        with os.fdopen(read_fd, 'rb', buffering=0) as reader, os.fdopen(write_fd, 'wb', buffering=0) as writer:
            session.process = types.SimpleNamespace(stdout=reader)
            writer.write(b'{"return":')
            with self.assertRaisesRegex(AssertionError, 'timed out'):
                session.qmp_line(time.monotonic() + .02)
            writer.write(b'{} }\nnext\n')
            self.assertEqual(session.qmp_line(time.monotonic() + 1), b'{"return":{} }')
            self.assertEqual(session.qmp_line(time.monotonic() + 1), b'next')

    def test_precomputed_outputs_have_expected_independent_native_values(self):
        with tempfile.TemporaryDirectory() as temporary:
            outputs = expected_outputs(pathlib.Path(temporary))
            self.assertEqual(native_writer(outputs['/Documents/async.bwr'])['text'], b'ALPHAZCD')
            self.assertEqual(native_sheet(outputs['/Documents/async.bsh']), {(0, 0): (2, '27.125')})
            self.assertEqual(outputs['/Documents/empty.csv'], b'')
            self.assertTrue(outputs['/Documents/document.rtf'].startswith(b'{\\rtf1'))
            self.assertTrue(outputs['/Documents/document.pdf'].startswith(b'%PDF-1.4'))

    def test_real_offline_profile_fixtures_and_immutable_metadata(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            seed_path = root / 'seed.img'; seed(seed_path)
            app = root / 'test.bex'; app.write_bytes(b'app fixture')
            build = root / 'build'; build.mkdir(); (build / 'boot.bin').write_bytes(bytes(512))
            (build / 'kernel.bin').write_bytes(b'kernel fixture')
            for profile, total, guest in (('default', 7 << 20, 64), ('large', 28 << 20, 128),
                                          ('floppy', 4096, 64)):
                with mock.patch('document_save_input_test.install_kernel'):
                    disk, originals, details = fixture(root, profile, build, app, seed_path,
                                                       {'/Documents/empty.csv': b''})
                self.assertEqual(details['payload_bytes'], total)
                self.assertEqual(details['guest_mib'], guest)
                nodes = load(disk.read())[2]
                verify_payloads(nodes, originals)
                manifest = nodes[resolve(nodes, '/Documents/document-check.bin')]['data']
                self.assertLessEqual(len(manifest), 4096)
                self.assertEqual(struct.unpack_from('<4I', manifest)[:3],
                                 (MANIFEST_MAGIC, 1, len(originals) + 1))
                modified = nodes
                verify_payloads(modified, originals)
                first = next(iter(originals))
                modified[first]['modified'] += 1
                with self.assertRaises(AssertionError):
                    verify_payloads(modified, originals)


if __name__ == '__main__':
    unittest.main()
