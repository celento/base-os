"""Host-only PS/2 BEX2 runner preparation/evidence tests. Never starts QEMU."""
import hashlib
from pathlib import Path
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from address_space_input_test import (VisibleText, expected_report, seed_disk,
                                      validate_stopped_files, wait_visible, main)
from make_stats_fixture import document
import init_data
import volume


class AddressSpaceInputTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.font = VisibleText(ROOT / 'src/font.h')

    def pixels(self, lines, x=27, y=39):
        pixels = np.full((390, 880, 3), 32, dtype=np.uint8)
        for number, text in enumerate(lines):
            mask = self.font.template(text)
            region = pixels[y + number * 19:y + number * 19 + 18, x:x + mask.shape[1]]
            region[mask] = 230
        return pixels

    def test_published_example_oracles_match_documented_unsigned_results(self):
        source = document()
        self.assertEqual(expected_report('array', source),
                         b'BEX2 workspace array\nInput bytes: 56812\nWords: 262144\n'
                         b'Seed: 4140131180\nChecksum: 3120168960\n')
        self.assertEqual(expected_report('index', source),
                         b'BEX2 workspace line index\nInput bytes: 56812\nIndex capacity: 262144\n'
                         b'Lines: 904\nOffset checksum: 25618244\n')

    def test_line_oracle_final_newline_and_empty_input(self):
        for source, lines, checksum in ((b'', 0, 0), (b'\n', 1, 0), (b'a\n', 1, 0),
                                        (b'a\nb', 2, 2), (b'\n\n', 2, 1)):
            result = expected_report('index', source)
            self.assertIn(f'Lines: {lines}\nOffset checksum: {checksum}\n'.encode(), result)
        with self.assertRaises(ValueError):
            expected_report('index', b'a\nb', workspace=4)
        with self.assertRaises(ValueError):
            expected_report('other', b'abc')

    def test_visible_glyph_locator_finds_full_exact_lines_at_unknown_origin(self):
        lines = ['BEX2 workspace array', 'Input bytes: 56812', 'Seed: 4140131180',
                 '/Documents/workspace-array-3.txt', 'Native task finished.']
        pixels = self.pixels(lines)
        for index, line in enumerate(lines):
            self.assertEqual(self.font.find(pixels, line), [[27, 39 + index * 19]])
        self.assertEqual(self.font.find(pixels, 'Seed: 4140131181'), [])
        self.assertEqual(self.font.find(pixels, '/Documents/workspace-array-4.txt'), [])
        # A single missing glyph pixel cannot establish a complete result line.
        mask = self.font.template(lines[0])
        y, x = np.argwhere(mask)[0]
        pixels[39 + y, 27 + x] = 32
        self.assertEqual(self.font.find(pixels, lines[0]), [])

    def test_threshold_matches_real_terminal_alpha_rounding(self):
        # Use the source font's original 4-bit values, not the detector templates,
        # and independent production gray ramp/mix arithmetic.
        import re
        source = (ROOT / 'src/font.h').read_text()
        body = re.search(r'edit_font_px\[95\]\[72\]\s*=\s*\{(.*?)\n\};', source, re.S)[1]
        packed = np.array([int(v, 16) for v in re.findall(r'0x([0-9a-fA-F]{2})', body)], dtype=np.uint8).reshape(95, 18, 4)
        levels = np.stack((packed >> 4, packed & 15), axis=-1).reshape(95, 18, 8)
        gray = np.array([i * 255 // 31 for i in range(32)])
        shades = []
        for alpha in range(16):
            raw = 230 if alpha == 15 else 32 + 198 * ((alpha * 256 + 7) // 15) // 256
            shades.append(int(gray[np.argmin(np.abs(gray - raw))]))
        for char in '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz:/-.':
            tile = np.array(shades, dtype=np.uint8)[levels[ord(char) - 32]]
            image = np.repeat(tile[:, :, None], 3, axis=2)
            self.assertTrue(np.array_equal(self.font.screenshot_mask(image), self.font.template(char)), char)

    def test_visible_evidence_requires_two_complete_matching_frames(self):
        lines = ['BEX2 workspace array', 'Native task finished.']
        path = '/Documents/workspace-array-2.txt'
        first = self.pixels([lines[0]])
        accepted = self.pixels(lines + [path])
        class Process:
            @staticmethod
            def poll(): return None
        class Session:
            process = Process()
            last_key = float('inf')
            def __init__(self, directory):
                self.directory = directory
                self.log = directory / 'serial.log'
                self.log.write_text('DESKTOP-READY\n')
                self.events = []
                self.frames = iter((first, accepted, accepted))
                self.count = 0
            def frame(self):
                self.count += 1
                return next(self.frames), float(self.count)
        with tempfile.TemporaryDirectory() as temporary, patch('address_space_input_test.time.sleep'):
            session = Session(Path(temporary))
            result = wait_visible(session, self.font, lines, 'complete', 20, [path])
            self.assertEqual(session.count, 3)
            self.assertEqual(result['alternative'], path)
            actual = np.array(Image.open(session.directory / 'complete.png').convert('RGB'))
            self.assertTrue(np.array_equal(actual, accepted))

    def test_both_disposable_profiles_contain_exact_apps_and_distinct_documents(self):
        apps = {'workspace-array.bex': b'app bytes', 'counter.bex': b'frozen bytes'}
        documents = {'stats-sample.txt': document(), 'index-source.txt': b'Other document\n'}
        modules = dict(init_data=init_data, volume=volume)
        with tempfile.TemporaryDirectory() as temporary:
            for profile in ('default', 'large'):
                path = Path(temporary) / (profile + '.img')
                nodes = seed_disk(path, profile, apps, documents, modules)
                self.assertEqual(volume.load(path.read_bytes())[2], nodes)
                self.assertEqual(path.stat().st_size, volume.data_layout(profile).sectors * 512)
                for parent, entries in (('Programs', apps), ('Documents', documents)):
                    for name, expected in entries.items():
                        self.assertEqual(nodes[volume.resolve(nodes, '/' + parent + '/' + name)]['data'], expected)
                with self.assertRaises(ValueError):
                    seed_disk(path, profile, apps, documents, modules)

    def test_prepare_only_never_enters_guest_execution(self):
        args = SimpleNamespace(build=ROOT / 'build', work=ROOT / 'unused-preparation', prepare_only=True)
        with patch('address_space_input_test.runtime_tools', return_value=(ROOT, {})), \
             patch('address_space_input_test.prepare', return_value={}) as prepared, \
             patch('address_space_input_test.run_profile', side_effect=AssertionError('No guest allowed')):
            main(args)
            prepared.assert_called_once()

    def test_execution_requires_explicit_coordinator_slot_flag(self):
        args = SimpleNamespace(build=ROOT / 'build', work=ROOT / 'unused-preparation',
                               prepare_only=False, qemu_slot_held=False)
        with patch('address_space_input_test.runtime_tools', return_value=(ROOT, {})), \
             patch('address_space_input_test.run_profile', side_effect=AssertionError('No guest allowed')):
            with self.assertRaisesRegex(ValueError, 'qemu-slot-held'):
                main(args)

    def test_stopped_validation_requires_exact_saved_bytes_and_unchanged_inputs(self):
        apps = {'counter.bex': b'frozen bytes'}
        documents = {'workspace-array-2.txt': b'report\n', 'source.txt': b'original\n'}
        report = dict(original_files={'/Programs/counter.bex': dict(bytes=12, sha256=hashlib.sha256(apps['counter.bex']).hexdigest())})
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'default.img'
            seed_disk(path, 'default', apps, documents, dict(init_data=init_data, volume=volume))
            result = validate_stopped_files(path, report, {'/Documents/workspace-array-2.txt': b'report\n'}, volume)
            self.assertEqual(result['/Documents/workspace-array-2.txt']['text'], 'report\n')
            with self.assertRaises(AssertionError):
                validate_stopped_files(path, report, {'/Documents/workspace-array-2.txt': b'wrong\n'}, volume)
            report['original_files']['/Programs/counter.bex']['bytes'] = 1
            with self.assertRaises(AssertionError):
                validate_stopped_files(path, report, {}, volume)


if __name__ == '__main__':
    unittest.main()
