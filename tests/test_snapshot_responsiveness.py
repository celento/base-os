"""Host checks for the normal-input snapshot collector; no guest hooks."""
import pathlib
import struct
import sys
import tempfile
import unittest
import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from snapshot_responsiveness_test import (COLORS, FIELDS, Session, fixture, fnv,
                                          snapshot_jobs, outstanding_snapshots, durable_report)
from volume import load, resolve, data_layout, encode_snapshot


class SnapshotEvidenceTest(unittest.TestCase):
    def test_visible_canvas_round_trip_at_both_normal_scales(self):
        values = (1, 5, 88, 0x12345678, 456, 7, 0x31505253, 4)
        for scale in (1, 2):
            pixels = np.full((720, 1280, 3), 33, dtype=np.uint8)
            x, y = 151, 139
            pixels[y:y + 100 * scale, x:x + 160 * scale] = 0
            for block, color in enumerate(COLORS):
                pixels[y:y + 8 * scale, x + block * 8 * scale:x + (block + 1) * 8 * scale] = color
            for row, value in enumerate(values):
                for bit in range(32):
                    if value & (1 << bit):
                        px, py = x + bit * 5 * scale, y + (12 + row * 10) * scale
                        pixels[py:py + 5 * scale, px:px + 4 * scale] = 255
            session = Session.__new__(Session)
            session.canvas = None
            session.frame = lambda keep: (pixels, 123.4)
            actual, _, wall = session.observe()
            self.assertEqual(actual, dict(zip(FIELDS, values)))
            self.assertEqual(session.canvas, (x, y, scale))
            self.assertEqual(wall, 123.4)
            pixels[y + 14 * scale, x + 2 * scale] = (123, 124, 125)
            with self.assertRaises(AssertionError):
                session.observe()

    def test_serial_matching_and_wrap(self):
        log = ('FS snapshot begin tick=FFFFFFFA generation=00000008\n'
               'FS snapshot end tick=00000004 generation=00000008 result=durable\n'
               'FS snapshot begin tick=00000005 generation=00000009\n')
        jobs = snapshot_jobs(log)
        self.assertAlmostEqual(jobs[0]['duration_seconds'], 10 / 70)
        self.assertEqual(jobs[0]['result'], 'durable')
        self.assertEqual(outstanding_snapshots(log), [jobs[1]])
        with self.assertRaises(AssertionError):
            snapshot_jobs('FS snapshot end tick=00000004 generation=00000008 result=error\n')

    def test_profiles_and_durable_report_gate(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = pathlib.Path(temporary)
            song, app = folder / 'song.mp3', folder / 'probe.bex'
            song.write_bytes(b'fixture media'); app.write_bytes(b'fixture program')
            for profile, size, count in (('default', 7 << 20, 4), ('large', 28 << 20, 2)):
                disk, original, details = fixture(folder, profile, song, app)
                nodes = load(disk.read_bytes())[2]
                self.assertEqual(nodes, original)
                self.assertEqual(details['payload_bytes'], size)
                self.assertEqual(len(details['file_lengths']), count)
                self.assertLess(details['initial_snapshot_bytes'], data_layout(profile).payload_limit - 65536)
                for name, content in (('trigger.bin', b'A'), ('probe.bin', b'P'), ('report.bin', bytes(1024))):
                    self.assertEqual(nodes[resolve(nodes, '/Documents/' + name)]['data'], content)
                for i, length in enumerate(details['file_lengths']):
                    content = nodes[resolve(nodes, f'/Documents/payload{i}.bin')]['data']
                    self.assertEqual(len(content), length)
                    self.assertEqual(fnv(content), details['manifest'][3 + i * 2])
                if profile == 'default':
                    nodes[resolve(nodes, '/Documents/report.bin')]['data'] = struct.pack('<I', 0x31505253) + bytes(1020)
                    header, payload = encode_snapshot(nodes, data_layout(profile), 2)
                    raw = bytearray(disk.read_bytes()); offset = data_layout(profile).lbas[1] * 512
                    raw[offset:offset + 512] = header.ljust(512, b'\0')
                    raw[offset + 512:offset + 512 + len(payload)] = payload
                    disk.write_bytes(raw)
                    begin = 'FS snapshot begin tick=00000001 generation=00000002\n'
                    self.assertEqual(durable_report(disk, begin), 0)
                    self.assertEqual(durable_report(disk, begin + 'FS snapshot end tick=00000005 generation=00000002 result=error\n'), 0)
                    self.assertEqual(durable_report(disk, begin + 'FS snapshot end tick=00000005 generation=00000002 result=durable\n'), 2)

    def test_guest_memory_observation_is_prohibited(self):
        with self.assertRaises(AssertionError):
            Session.__new__(Session).memory(0, 1)


if __name__ == '__main__':
    unittest.main()
