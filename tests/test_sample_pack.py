import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import init_data  # noqa: E402
import sample_pack  # noqa: E402
import volume  # noqa: E402


class SamplePackTests(unittest.TestCase):
    files = {'Documents/Sample/a.txt': b'hello', 'Pictures/Sample/empty.png': b'', 'top.bin': bytes(range(256)) * 9}

    def test_round_trip_in_the_second_slot_tail(self):
        for profile in ('default', 'large'):
            with self.subTest(profile=profile):
                layout = volume.data_layout(profile)
                image = bytearray(volume.data_marker(profile).ljust(layout.sectors * 512, b'\0'))
                first = sample_pack.install(image, self.files, layout.lbas[1])
                self.assertGreater(first, layout.lbas[1])
                self.assertEqual(sample_pack.locate(image), (first, layout.sectors - 1 - first))
                self.assertEqual(sample_pack.decode(bytes(image)), self.files)
                stripped = sample_pack.without_pack(bytes(image))
                self.assertFalse(any(stripped[512:]))
                self.assertEqual(stripped[:512], image[:512])

    def test_damage_is_detected(self):
        layout = volume.data_layout()
        image = bytearray(layout.sectors * 512)
        first = sample_pack.install(image, self.files, layout.lbas[1])
        image[first * 512 + 3] ^= 1
        with self.assertRaises(ValueError):
            sample_pack.decode(bytes(image))
        image[-512] ^= 1
        self.assertIsNone(sample_pack.locate(image))

    def test_names_must_fit_snapshot_volumes(self):
        for name in ('Documents/' + 'x' * 24, 'a//b', '/a', 'a/../b'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                sample_pack.encode({name: b''})

    def test_default_files(self):
        files = sample_pack.default_files()
        folders = {name.rsplit('/', 1)[0] for name in files}
        self.assertEqual(folders, {'Documents/Sample', 'Pictures/Sample', 'Media/Sample'})
        self.assertTrue(all(0 < len(data) <= volume.DATA_FILE_LIMIT for data in files.values()))
        sample_pack.encode(files)

    def test_init_data_samples_keep_the_marker_and_blank_state(self):
        with tempfile.TemporaryDirectory() as temp:
            image = pathlib.Path(temp) / 'data.img'
            result = subprocess.run([sys.executable, str(ROOT / 'tools/init_data.py'), str(image), '--samples'],
                                    capture_output=True, text=True, check=True)
            self.assertIn('with the sample pack', result.stdout)
            data = image.read_bytes()
            self.assertEqual(data[:512], volume.data_marker())
            self.assertEqual(sample_pack.decode(data), sample_pack.default_files())
            info = subprocess.run([sys.executable, str(ROOT / 'tools/volume.py'), str(image), 'info'],
                                  capture_output=True, text=True, check=True)
            self.assertIn('boot BaseOS once', info.stdout)
            self.assertFalse(init_data.initialize(image, samples=self.files))
            self.assertEqual(image.read_bytes(), data)


if __name__ == '__main__':
    unittest.main()
