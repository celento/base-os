"""Reference and state tests for the real MPEG-1 playback implementation.

Requires ffmpeg/ffprobe. No downloaded content and no malformed input corpus:
all media is valid footage generated locally with FFmpeg. YUV420 output
from the BaseOS decoder is compared to FFmpeg in display order, every frame and
plane, including the final delayed reference picture.
"""
from fractions import Fraction
import hashlib
import math
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from video_fixture import make_fixture, decode_reference, probe_fixture, run_ffmpeg


@unittest.skipUnless(shutil.which('ffmpeg') and shutil.which('ffprobe'),
                     'MPEG-1 reference tests require ffmpeg and ffprobe')
class VideoTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix='baseos-video-host-')
        cls.directory = pathlib.Path(cls.temporary.name)
        cls.executable = cls.directory / 'video-host'
        compiler = shutil.which('clang') or shutil.which('cc')
        if compiler is None:
            raise unittest.SkipTest('MPEG-1 host tests require a C compiler')
        subprocess.run([compiler, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra',
                        '-Werror', '-fsanitize=address,undefined', '-DVIDEO_HOST_TEST','-DMEDIA_MP3_HOST_TEST',
                        '-I', str(ROOT / 'src'), str(ROOT / 'tests/video_host.c'),
                        str(ROOT / 'src/video.c'), str(ROOT / 'src/media_mp3.c'), '-lm', '-o', str(cls.executable)],
                       check=True)
        cls.environment = dict(os.environ)
        cls.environment['ASAN_OPTIONS'] = 'detect_leaks=0'
        cls.environment['UBSAN_OPTIONS'] = 'halt_on_error=1:print_stacktrace=1'

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def check_reference(self, actual, reference, width, height, frames):
        actual, reference = actual.read_bytes(), reference.read_bytes()
        sizes = [width * height, width * height // 4, width * height // 4]
        frame_size = sum(sizes)
        self.assertEqual(len(reference), frames * frame_size)
        self.assertEqual(len(actual), len(reference), 'Dropped or extra decoded frames')
        worst_mae, worst_rms, maximum = 0.0, 0.0, 0
        source_hashes = set()
        violations = []
        for frame in range(frames):
            offset = frame * frame_size
            source_hashes.add(hashlib.sha256(reference[offset:offset + frame_size]).digest())
            for name, size in zip(('Y', 'Cb', 'Cr'), sizes):
                errors = [abs(a - b) for a, b in zip(actual[offset:offset + size],
                                                   reference[offset:offset + size])]
                mae = sum(errors) / size
                rms = math.sqrt(sum(error * error for error in errors) / size)
                peak = max(errors)
                context = f'frame {frame}, {name}: MAE={mae:.4f}, RMS={rms:.4f}, max={peak}'
                # Scalar integer IDCT and predicted-frame rounding differ by
                # low single-digit sample values. These per-plane/per-frame
                # limits allow that rounding while rejecting chroma swaps,
                # wrong display order, stale frames, or a damaged final block.
                # Measured across 40-second fractional-rate fixtures: worst
                # chroma plane MAE 0.6745/RMS 1.0496, with peak only 4. The
                # peak bound stays strict even for end-of-stream pictures.
                if mae > 0.75 or rms > 1.2 or peak > 10:
                    violations.append((peak, rms, mae, context))
                worst_mae = max(worst_mae, mae)
                worst_rms = max(worst_rms, rms)
                maximum = max(maximum, peak)
                offset += size
        self.assertEqual(len(source_hashes), frames, 'Fixture must change every frame')
        self.assertFalse(violations, 'Reference mismatches: ' + '; '.join(
            item[3] for item in sorted(violations, reverse=True)[:10]))
        print(f'  FFmpeg reference: {frames} full Y/Cb/Cr frames, worst plane MAE '
              f'{worst_mae:.4f}, RMS {worst_rms:.4f}, peak {maximum}', flush=True)

    def fixture_case(self, name, width=320, height=240, fps='25', frames=75,
                     bframes=2, controls=False, audio=False, clock_wrap=False):
        source = make_fixture(self.directory, width, height, fps, frames, name,
                              bframes=bframes, audio=audio)
        self.assertLessEqual(source.stat().st_size, 2 * 1024 * 1024)
        metadata = probe_fixture(source)
        self.assertEqual(metadata['codec'], 'mpeg1video')
        self.assertEqual((metadata['width'], metadata['height']), (width, height))
        self.assertEqual(metadata['frames'], frames)
        self.assertEqual(Fraction(metadata['fps']), Fraction(fps))
        types = set(metadata['picture_types'])
        self.assertIn('I', types)
        if frames > 12:
            self.assertIn('P', types)
            self.assertEqual('B' in types, bframes > 0)
        reference = decode_reference(source)
        actual = source.with_suffix('.baseos.yuv')
        rate = Fraction(fps)
        mode = int(controls) | (int(audio) << 1) | (int(clock_wrap) << 2)
        subprocess.run([str(self.executable), str(source), str(actual), str(width),
                        str(height), str(rate.numerator), str(rate.denominator),
                        str(frames), str(mode)], check=True, env=self.environment)
        self.check_reference(actual, reference, width, height, frames)
        return source, reference

    def test_25fps_ipb_and_controls(self):
        source, reference = self.fixture_case('ipb-25', controls=True)
        # The same content also runs without virtual pauses to check cadence.
        actual = source.with_suffix('.cadence.yuv')
        subprocess.run([str(self.executable), str(source), str(actual), '320', '240',
                        '25', '1', '75', '0'], check=True, env=self.environment)
        self.check_reference(actual, reference, 320, 240, 75)

    def test_fractional_ntsc_cadence(self):
        # Virtual time advances 40 seconds: rounding 30000/1001 to 30 would
        # create a multi-tick drift. Wall-clock execution remains fast.
        self.fixture_case('ntsc', 64, 48, '30000/1001', 1200, clock_wrap=True)

    def test_fractional_film_cadence(self):
        self.fixture_case('film', 64, 48, '24000/1001', 960)

    def test_non_macroblock_dimensions(self):
        self.fixture_case('padded', 318, 238, '24', 48)

    def test_maximum_supported_dimensions(self):
        self.fixture_case('vga', 640, 480, '30', 24)

    def test_single_intra_frame(self):
        self.fixture_case('single', 160, 120, '25', 1, bframes=0)

    def tiny_sequence_case(self, name, intra=False, non_intra=False):
        # A complete solid-color picture needs only 41 ES bytes with the
        # current FFmpeg encoder. MPEG-PS padding must not hide an overlarge
        # minimum sequence-header requirement in the elementary decoder.
        source = self.directory / (name + '.mpg')
        matrix_options = []
        if intra:
            matrix_options += ['-intra_matrix', ','.join(['16'] * 64)]
        if non_intra:
            matrix_options += ['-inter_matrix', ','.join(['17'] * 64)]
        run_ffmpeg(['-f', 'lavfi', '-i', 'color=c=red:size=16x16:rate=25',
                    '-frames:v', '1', '-an', '-c:v', 'mpeg1video', '-q:v', '3',
                    '-g', '12', '-bf', '0', '-threads', '1', '-flags', '+bitexact',
                    '-fflags', '+bitexact', *matrix_options, '-f', 'mpeg', source])
        self.assertTrue(source.read_bytes().startswith(b'\x00\x00\x01\xba'))
        metadata = probe_fixture(source)
        self.assertEqual(metadata['codec'], 'mpeg1video')
        self.assertEqual((metadata['width'], metadata['height']), (16, 16))
        self.assertEqual(metadata['frames'], 1)
        self.assertEqual(Fraction(metadata['fps']), Fraction(25))
        self.assertEqual(metadata['picture_types'], ['I'])

        elementary = source.with_suffix('.m1v')
        run_ffmpeg(['-i', source, '-map', '0:v:0', '-c:v', 'copy',
                    '-f', 'mpeg1video', elementary])
        data = elementary.read_bytes()
        self.assertTrue(data.startswith(b'\x00\x00\x01\xb3'))
        if not (intra and non_intra):
            self.assertLess(len(data), 140, 'Fixture must exercise the tiny ES case')
        # Verify the encoded flags and each optional matrix itself, rather
        # than assuming the encoder honored the requested options. The first
        # flag follows 62 fixed bits; the second follows the first matrix.
        bits = ''.join(f'{byte:08b}' for byte in data[4:])
        position = 62
        for present, value in ((intra, 16), (non_intra, 17)):
            self.assertEqual(int(bits[position]), int(present))
            position += 1
            if present:
                matrix = [int(bits[offset:offset + 8], 2)
                          for offset in range(position, position + 512, 8)]
                self.assertEqual(matrix, [value] * 64)
                position += 512
        self.assertEqual(position % 8, 0)
        header_end = 4 + position // 8
        self.assertEqual(data[header_end:header_end + 3], b'\x00\x00\x01')
        reference = decode_reference(source)
        actual = source.with_suffix('.baseos.yuv')
        subprocess.run([str(self.executable), str(source), str(actual), '16', '16',
                        '25', '1', '1', '0'], check=True, env=self.environment)
        self.check_reference(actual, reference, 16, 16, 1)
        print(f'  Tiny complete MPEG-1 ES: {len(data)} bytes; '
              f'intra matrix={intra}, non-intra matrix={non_intra}', flush=True)

    def test_tiny_single_intra_frame(self):
        self.tiny_sequence_case('tiny-default')

    def test_tiny_optional_quantization_matrices(self):
        for name, intra, non_intra in [('intra', True, False),
                                        ('non-intra', False, True),
                                        ('both', True, True)]:
            with self.subTest(matrices=name):
                self.tiny_sequence_case('tiny-' + name, intra, non_intra)

    def test_multiplexed_audio_is_detected(self):
        self.fixture_case('with-audio', 160, 120, '25', 50, audio=True)

    def test_valid_unsupported_formats(self):
        for name, width, codec in [('mpeg2', 320, 'mpeg2video'),
                                    ('too-wide', 642, 'mpeg1video')]:
            with self.subTest(name=name):
                source = make_fixture(self.directory, width, 240, '25', 12,
                                      name, codec=codec)
                metadata = probe_fixture(source)
                self.assertEqual(metadata['codec'], codec)
                self.assertEqual(metadata['width'], width)
                self.assertEqual(metadata['frames'], 12)
                subprocess.run([str(self.executable), '--reject', str(source)],
                               check=True, env=self.environment)

    def test_non_square_pixel_aspect(self):
        source=make_fixture(self.directory,352,240,'25',12,'aspect',sar='10/11')
        metadata=probe_fixture(source)
        # MPEG-1 stores inverse pixel aspect. FFmpeg rounds the requested SAR
        # to sequence code12, which denotes actual displayed SAR200/219.
        self.assertEqual(metadata['sample_aspect_ratio'],'200:219')
        reference=decode_reference(source);actual=source.with_suffix('.baseos.yuv')
        subprocess.run([str(self.executable),str(source),str(actual),'352','240','25','1','12','0','200','219'],
                       check=True,env=self.environment)
        self.check_reference(actual,reference,352,240,12)

    def test_without_b_frames(self):
        self.fixture_case('ip-only', 96, 64, '25', 50, bframes=0)


if __name__ == '__main__':
    unittest.main()
