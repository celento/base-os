"""Independent MP2 PCM reference and MPEG A/V state/timing integration tests.

Every input is a valid, locally generated MPEG program stream. PCM comparison
covers every decoded sample, including encoder priming and the final MP2 frame.
No phase search, arbitrary trim, resampling, or gain correction is applied.
"""
from array import array
from fractions import Fraction
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
from mpeg_av_fixture import make_av_fixture, probe_av_fixture, decode_audio_reference
from video_fixture import decode_reference
import test_video as video_reference


@unittest.skipUnless(shutil.which('ffmpeg') and shutil.which('ffprobe'),
                     'MPEG A/V reference tests require ffmpeg and ffprobe')
class MpegAvTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix='baseos-mpeg-av-')
        cls.directory = pathlib.Path(cls.temporary.name)
        cls.executable = cls.directory / 'mpeg-av-host'
        compiler = shutil.which('clang') or shutil.which('cc')
        if compiler is None:
            raise unittest.SkipTest('MPEG A/V tests require a C compiler')
        subprocess.run([compiler, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra',
                        '-Werror', '-fsanitize=address,undefined', '-DVIDEO_HOST_TEST',
                        '-DMEDIA_MP3_HOST_TEST',
                        '-I', str(ROOT / 'src'), str(ROOT / 'tests/mpeg_av_host.c'),
                        str(ROOT / 'src/video.c'), str(ROOT / 'src/media_mp3.c'),
                        '-lm', '-o', str(cls.executable)],
                       check=True)
        cls.environment = dict(os.environ)
        cls.environment['ASAN_OPTIONS'] = 'detect_leaks=0'
        cls.environment['UBSAN_OPTIONS'] = 'halt_on_error=1:print_stacktrace=1'

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def check_pcm(self, actual_path, reference_path, lead, mono):
        actual = array('h', actual_path.read_bytes())
        reference = array('h', reference_path.read_bytes())
        if sys.byteorder != 'little':
            actual.byteswap()
            reference.byteswap()
        reference = array('h', [0]) * (lead * 2) + reference
        self.assertEqual(len(actual), len(reference), 'Missing or extra PCM frames, including tail')
        self.assertEqual(actual[:lead * 2], array('h', [0]) * (lead * 2),
                         'Initial PTS delay must be exact silence')
        if mono:
            self.assertEqual(actual[0::2], actual[1::2], 'Mono output must duplicate both channels')
        else:
            self.assertNotEqual(actual[0::2], actual[1::2], 'Stereo channels must remain distinct')
        worst_rms, peak = 0, 0
        for start in range(0, len(actual), 1152 * 2):
            end = min(start + 1152 * 2, len(actual))
            errors = [abs(a - b) for a, b in zip(actual[start:end], reference[start:end])]
            rms = math.sqrt(sum(e * e for e in errors) / len(errors))
            block_peak = max(errors)
            worst_rms, peak = max(worst_rms, rms), max(peak, block_peak)
            # Scalar floating-point synthesis versus FFmpeg's integer synthesis
            # permits low-LSB rounding only, on every block including the tail.
            self.assertLessEqual(rms, 2.0, f'PCM block {start // 2304} RMS {rms:.4f}')
            self.assertLessEqual(block_peak, 12, f'PCM block {start // 2304} peak {block_peak}')
        self.assertGreater(max(abs(v) for v in reference[-2304:]), 100,
                           'Tail reference must contain audible data')
        print(f'  FFmpeg PCM: {len(actual) // 2} stereo frames, {lead} PTS-silence frames, '
              f'worst block RMS {worst_rms:.4f}, peak {peak}', flush=True)

    def fixture_case(self, name, rate=44100, channels=2, mode=0, clock_wrap=False,
                     **options):
        source = make_av_fixture(self.directory, name=name, rate=rate,
                                 channels=channels, **options)
        self.assertLessEqual(source.stat().st_size, 2 * 1024 * 1024)
        metadata = probe_av_fixture(source)
        self.assertEqual(metadata['codec'], 'mpeg1video')
        self.assertEqual(metadata['sample_rate'], rate)
        self.assertEqual(metadata['source_channels'], channels)
        reference = decode_audio_reference(source, channels)
        self.assertEqual(reference.stat().st_size % 4, 0)
        audio_frames = reference.stat().st_size // 4
        if metadata['audio_codec'] == 'mp2':
            self.assertEqual(audio_frames, metadata['audio_packets'] * 1152,
                             'Reference must retain all complete MP2 frames')
        actual = source.with_suffix('.baseos.s16')
        fps = Fraction(metadata['fps'])
        subprocess.run([str(self.executable), str(source), str(actual),
                        str(metadata['width']), str(metadata['height']),
                        str(fps.numerator), str(fps.denominator), str(metadata['frames']),
                        str(rate), str(channels), str(audio_frames),
                        str(metadata['audio_lead_frames']), str(metadata['video_start_ms']),
                        str(mode), str(int(clock_wrap))], check=True, env=self.environment)
        video_reference.VideoTests.check_reference(
            self, pathlib.Path(str(actual) + '.yuv'), decode_reference(source),
            metadata['width'], metadata['height'], metadata['frames'])
        if mode < 2:
            self.check_pcm(actual, reference, metadata['audio_lead_frames'], channels == 1)
        else:
            self.assertEqual(actual.stat().st_size, 0)

    def test_stereo_44100_controls_and_audio_master(self):
        self.fixture_case('stereo-44100', mode=1, clock_wrap=True)

    def test_all_mpeg1_sample_rates_and_channels(self):
        for rate, channels in ((32000, 1), (32000, 2), (44100, 1), (48000, 1), (48000, 2)):
            with self.subTest(rate=rate, channels=channels):
                self.fixture_case(f'audio-{rate}-{channels}', rate=rate, channels=channels)

    def test_audio_initial_pts_delay(self):
        self.fixture_case('audio-later', audio_offset=0.2)

    def test_video_initial_pts_delay(self):
        self.fixture_case('video-later', audio_offset=-0.2)

    def test_video_finishes_after_audio_tail(self):
        self.fixture_case('shorter-audio', audio_extra=-0.45)

    def test_valid_out_of_range_pts_plays_silent_video(self):
        self.fixture_case('unsupported-offset', audio_offset=2.1, mode=3)

    def test_missing_sound_card_preserves_video(self):
        self.fixture_case('no-device', mode=2)

    def test_valid_unsupported_audio_preserves_video(self):
        for name, rate, codec in (('layer-three', 44100, 'mp3'),
                                   ('mpeg2-layer-two', 22050, 'mp2')):
            with self.subTest(codec=codec, rate=rate):
                self.fixture_case(name, rate=rate, audio_codec=codec, mode=3)


if __name__ == '__main__':
    unittest.main()
