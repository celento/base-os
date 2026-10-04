"""Original MPEG-1/MP2 A/V fixtures with independent FFmpeg PCM references.

Audio is synthesized locally with distinct left/right tones, a chirp, and a
slow envelope. No downloaded assets, malformed inputs, or sample trimming.
Initial alignment comes from actual program-stream PTS values. Both encoder
priming and the final padded MP2 frame remain in the reference output.
"""
import argparse
from array import array
from fractions import Fraction
import json
import math
import pathlib
import subprocess
import sys
import wave

from video_fixture import source_frame, run_ffmpeg, probe_fixture


def make_av_fixture(directory, name='av', rate=44100, channels=2, frames=50,
                    fps='25', width=96, height=64, audio_extra=0.12,
                    audio_offset=0, audio_codec='mp2'):
    directory = pathlib.Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    original = directory / (name + '.source.yuv')
    with original.open('wb') as output:
        for frame in range(frames):
            output.write(source_frame(width, height, frame))
    wav = directory / (name + '.source.wav')
    sample_count = round((float(Fraction(frames, 1) / Fraction(fps)) + audio_extra) * rate)
    samples = array('h')
    for index in range(sample_count):
        t = index / rate
        envelope = 0.65 + 0.25 * math.sin(2 * math.pi * 1.7 * t)
        for channel in range(channels):
            base = 271 if channel == 0 else 683
            # Distinct non-periodic channels expose swaps, stale blocks, and
            # phase shifts without clipping or ambiguous constant silence.
            value = (math.sin(2 * math.pi * base * t) * 6500
                     + math.sin(2 * math.pi * (113 * t + (170 + channel * 61) * t * t)) * 3200
                     + math.sin(2 * math.pi * (2311 + channel * 547) * t) * 1700)
            samples.append(round(value * envelope))
    if sys.byteorder != 'little':
        samples.byteswap()
    with wave.open(str(wav), 'wb') as output:
        output.setnchannels(channels)
        output.setsampwidth(2)
        output.setframerate(rate)
        output.writeframes(samples.tobytes())
    encoded = directory / (name + '.mpg')
    run_ffmpeg(['-f', 'rawvideo', '-pixel_format', 'yuv420p', '-video_size',
                f'{width}x{height}', '-framerate', fps, '-i', original,
                '-itsoffset', str(audio_offset), '-i', wav,
                '-map', '0:v:0', '-map', '1:a:0', '-c:v', 'mpeg1video',
                '-q:v', '3', '-g', '12', '-bf', '2', '-sc_threshold', '0',
                '-c:a', audio_codec, '-b:a', '192k' if channels == 2 and rate >= 32000 else '96k',
                '-ar', rate, '-ac', channels, '-threads', '1',
                '-flags', '+bitexact', '-fflags', '+bitexact', '-f', 'mpeg', encoded])
    return encoded


def probe_av_fixture(encoded):
    result = subprocess.run(['ffprobe', '-v', 'error', '-show_streams',
                             '-show_packets', '-of', 'json', str(encoded)],
                            capture_output=True, text=True, check=True)
    data = json.loads(result.stdout)
    video = next(s for s in data['streams'] if s['codec_type'] == 'video')
    audio = next(s for s in data['streams'] if s['codec_type'] == 'audio')
    packets = data['packets']
    first_audio = next(p for p in packets if p['stream_index'] == audio['index'])
    first_video = next(p for p in packets if p['stream_index'] == video['index'])
    audio_pts = int(first_audio['pts']) * Fraction(audio['time_base'])
    video_pts = int(first_video['pts']) * Fraction(video['time_base'])
    rate = int(audio['sample_rate'])
    delta = audio_pts - video_pts
    # Adapter intentionally uses integer sample/ms floor conversion. Derive
    # expected silence from timestamps, never cross-correlate to hide a shift.
    lead = int(max(delta, 0) * rate)
    video_start = int(max(-delta, 0) * 1000)
    video_metadata = probe_fixture(encoded)
    audio_packets = [p for p in packets if p['stream_index'] == audio['index']]
    return dict(video_metadata, audio_codec=audio['codec_name'], sample_rate=rate,
                source_channels=int(audio['channels']), audio_pts=str(audio_pts),
                video_pts=str(video_pts), audio_lead_frames=lead,
                video_start_ms=video_start, audio_packets=len(audio_packets),
                audio_frame_samples=(1152 if audio['codec_name'] == 'mp2' else None))


def decode_audio_reference(encoded, channels, output=None):
    encoded = pathlib.Path(encoded)
    output = pathlib.Path(output) if output else encoded.with_suffix('.reference.s16')
    mono = ['-af', 'pan=stereo|c0=c0|c1=c0'] if channels == 1 else []
    run_ffmpeg(['-threads', '1', '-i', encoded, '-map', '0:a:0', '-vn',
                *mono, '-c:a', 'pcm_s16le', '-f', 's16le', output])
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=pathlib.Path)
    parser.add_argument('--name', default='av')
    parser.add_argument('--rate', type=int, default=44100)
    parser.add_argument('--channels', type=int, choices=(1, 2), default=2)
    parser.add_argument('--audio-offset', type=float, default=0)
    args = parser.parse_args()
    encoded = make_av_fixture(args.directory, name=args.name, rate=args.rate,
                              channels=args.channels, audio_offset=args.audio_offset)
    metadata = probe_av_fixture(encoded)
    reference = decode_audio_reference(encoded, args.channels)
    metadata.update(encoded=str(encoded), reference=str(reference),
                    decoded_audio_frames=reference.stat().st_size // 4,
                    bytes=encoded.stat().st_size)
    print(json.dumps(metadata, indent=2))


if __name__ == '__main__':
    main()
