#!/usr/bin/env python3
"""Build the original Harbor audio/video examples offline (Python + Pillow + FFmpeg).

The composition, synthesis, and drawn scenery are original BaseOS project work.
SPDX-License-Identifier: MIT
"""
import argparse
from array import array
import hashlib
import json
import math
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import wave

from PIL import Image, ImageDraw

RATE = 44100
WIDTH, HEIGHT, FPS = 320, 240, 25
AUDIO_SECONDS, VIDEO_SECONDS = 18, 9


def ffmpeg(*args):
    subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-nostdin',
                    '-y', *map(str, args)], check=True)


def soundtrack(path):
    """Six original three-second harmony cells, softly plucked with a low pad."""
    count = AUDIO_SECONDS * RATE
    left, right = array('f', [0]) * count, array('f', [0]) * count
    chords = ((50, 57, 61, 66), (47, 54, 57, 62), (43, 54, 59, 62),
              (40, 55, 59, 62), (45, 55, 59, 64), (38, 54, 57, 62))

    def note(midi, start, seconds, amplitude, pan, pluck):
        length, offset = round(seconds * RATE), round(start * RATE)
        frequency = 440 * 2 ** ((midi - 69) / 12)
        step = 2 * math.pi * frequency / RATE
        gain_l = math.cos((pan + 1) * math.pi / 4) * amplitude
        gain_r = math.sin((pan + 1) * math.pi / 4) * amplitude
        for i in range(min(length, count - offset)):
            t = i / RATE
            attack = min(1, t / (0.012 if pluck else 0.18))
            release = min(1, (length - i) / (RATE * 0.35))
            envelope = attack * release * (math.exp(-3.8 * t) if pluck else 0.75)
            phase = i * step
            # Harmonics add a wooden/plucked character without external samples.
            sample = (math.sin(phase) + 0.23 * math.sin(2 * phase)
                      + 0.06 * math.sin(3 * phase)) * envelope
            left[offset + i] += sample * gain_l
            right[offset + i] += sample * gain_r

    for bar, chord in enumerate(chords):
        start = bar * 3.0
        for voice, pitch in enumerate(chord):
            note(pitch, start, 2.95, 0.046 if voice == 0 else 0.021,
                 (-0.4, 0.35, -0.15, 0.5)[voice], False)
        order = (1, 2, 3, 2)
        for beat, voice in enumerate(order):
            pitch = chord[voice] + 12
            pan = (-0.32, 0.3, -0.12, 0.22)[beat]
            note(pitch, start + beat * 0.75, 1.15, 0.12, pan, True)
            note(pitch, start + beat * 0.75 + 0.27, 0.8, 0.025, -pan, True)

    peak = max(max(map(abs, left)), max(map(abs, right)))
    gain = 0.70 * 32767 / peak
    pcm = array('h')
    for i, (l, r) in enumerate(zip(left, right)):
        fade = min(1, i / (RATE * 0.12), (count - 1 - i) / (RATE * 1.1))
        pcm.extend((round(l * gain * fade), round(r * gain * fade)))
    if sys.byteorder != 'little':
        pcm.byteswap()
    with wave.open(str(path), 'wb') as output:
        output.setnchannels(2)
        output.setsampwidth(2)
        output.setframerate(RATE)
        output.writeframes(pcm.tobytes())


def scene(index):
    """An original cut-paper harbor: drifting clouds, sunset, sailboat and ripples."""
    scale = 3
    time = index / FPS
    canvas = Image.new('RGB', (WIDTH * scale, HEIGHT * scale))
    draw = ImageDraw.Draw(canvas)

    def polygon(points, color):
        draw.polygon([(round(x * scale), round(y * scale)) for x, y in points], fill=color)

    def ellipse(box, color):
        draw.ellipse(tuple(round(v * scale) for v in box), fill=color)

    def line(points, color, width=1):
        draw.line([(round(x * scale), round(y * scale)) for x, y in points],
                  fill=color, width=max(1, round(width * scale)))

    top, horizon = (50, 74, 103), (238, 167, 132)
    for y in range(HEIGHT * scale):
        amount = min(1, y / (153 * scale))
        color = tuple(round(a + (b - a) * amount) for a, b in zip(top, horizon))
        draw.line((0, y, WIDTH * scale, y), fill=color)

    sun_y = 100 + time * 1.5
    ellipse((197, sun_y - 29, 255, sun_y + 29), (245, 194, 139))
    for x, y, length in ((20, 58, 66), (126, 34, 38), (269, 74, 42)):
        shift = time * 1.8
        polygon(((x + shift, y), (x + length + shift, y),
                 (x + length - 11 + shift, y + 3), (x - 9 + shift, y + 3)),
                (194, 153, 147))

    for base, amplitude, phase, color in ((137, 21, 0.8, (105, 119, 135)),
                                         (154, 18, 2.8, (63, 97, 118))):
        ridge = [(x, base + math.sin(x / 61 + phase) * amplitude
                  + math.sin(x / 28 + phase) * 4) for x in range(-4, WIDTH + 5, 4)]
        polygon([(-4, 182), *ridge, (WIDTH + 4, 182)], color)

    polygon(((0, 160), (WIDTH, 160), (WIDTH, HEIGHT), (0, HEIGHT)), (39, 85, 105))
    for row in range(12):
        y = 163 + row * 6
        x = 227 + math.sin(time * 1.1 + row * 1.7) * (2 + row * 0.5)
        half = 5 + row * 2.8
        line(((x - half, y), (x + half, y)), (150, 142, 122), 1.3)
    for row in range(7):
        for column in range(5):
            x = column * 72 + (row % 2) * 21 + math.sin(time * 0.9 + row) * 6 - 20
            y = 174 + row * 10
            line(((x, y), (x + 13 + row * 2, y)), (59, 107, 122), 0.8)

    # Small boat crosses the harbor while the sail gently rocks about its mast.
    x = 69 + time * 13
    y = 189 + math.sin(time * 2) * 1.15
    sway = math.sin(time * 1.4) * 1.8
    line(((x - 40, y + 8), (x - 17, y + 8)), (97, 130, 134), 0.8)
    line(((x - 32, y + 11), (x - 8, y + 11)), (74, 113, 126), 0.8)
    polygon(((x - 23, y), (x + 22, y), (x + 14, y + 8), (x - 15, y + 8)),
            (27, 50, 67))
    line(((x, y), (x + sway, y - 54)), (223, 198, 158), 1.3)
    polygon(((x - 3 + sway, y - 50), (x - 3, y - 4), (x - 24, y - 4)),
            (242, 224, 179))
    polygon(((x + 2 + sway, y - 43), (x + 2, y - 4), (x + 19, y - 4)),
            (212, 169, 136))

    # A quiet foreground shore grounds the scene; no fonts or imported artwork.
    polygon(((0, 229), (32, 224), (62, 228), (99, 240), (0, 240)), (26, 57, 72))
    for x, height in ((17, 17), (21, 24), (26, 14)):
        line(((x, 230), (x - 3, 230 - height)), (26, 57, 72), 1.4)
    return canvas.resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS)


def verify(path, video):
    probe = subprocess.run(['ffprobe', '-v', 'error', '-count_frames',
                            '-show_streams', '-show_format', '-of', 'json', str(path)],
                           check=True, capture_output=True, text=True)
    data = json.loads(probe.stdout)
    audio = next(s for s in data['streams'] if s['codec_type'] == 'audio')
    assert audio['codec_name'] == ('mp2' if video else 'mp3')
    assert audio['sample_rate'] == str(RATE) and audio['channels'] == 2
    if not video:
        assert AUDIO_SECONDS <= float(data['format']['duration']) < AUDIO_SECONDS + 0.1
    if video:
        stream = next(s for s in data['streams'] if s['codec_type'] == 'video')
        assert data['format']['format_name'] == 'mpeg'
        assert stream['codec_name'] == 'mpeg1video'
        assert (stream['width'], stream['height']) == (WIDTH, HEIGHT)
        assert stream['avg_frame_rate'] == f'{FPS}/1'
        # FFprobe may infer r_frame_rate=50 from MPEG-1 field timestamps.
        # The sequence header and decoded picture count are authoritative.
        source = path.read_bytes()
        assert source[:4] == b'\x00\x00\x01\xba' and source[4] >> 4 == 2
        header = source.index(b'\x00\x00\x01\xb3')
        assert source[header + 7] & 15 == 3  # MPEG-1 rate code 3 = 25 fps
        assert int(stream['nb_read_frames']) == VIDEO_SECONDS * FPS
    # Decode every packet in both streams, treating decoder errors as failures.
    ffmpeg('-xerror', '-i', path, '-map', '0', '-f', 'null', '-')
    assert path.stat().st_size < 2 * 1024 * 1024
    return {'file': path.name, 'bytes': path.stat().st_size,
            'duration': data['format']['duration'],
            'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
            'streams': [{k: s[k] for k in ('codec_name', 'width', 'height',
                         'avg_frame_rate', 'sample_rate', 'channels', 'nb_read_frames') if k in s}
                        for s in data['streams']]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', nargs='?', type=Path, default=Path('assets/examples'))
    args = parser.parse_args()
    for binary in ('ffmpeg', 'ffprobe'):
        if not shutil.which(binary):
            parser.error(f'{binary} is required')
    args.output.mkdir(parents=True, exist_ok=True)
    audio, video = args.output / 'harbor.mp3', args.output / 'harbor.mpg'
    with tempfile.TemporaryDirectory(prefix='baseos-media-') as directory:
        temp = Path(directory)
        wav, raw = temp / 'harbor.wav', temp / 'harbor.rgb'
        soundtrack(wav)
        with raw.open('wb') as output:
            for index in range(VIDEO_SECONDS * FPS):
                output.write(scene(index).tobytes())
        ffmpeg('-i', wav, '-map_metadata', '-1', '-c:a', 'libmp3lame', '-b:a', '80k',
               '-ar', RATE, '-ac', '2', '-write_xing', '0', '-id3v2_version', '0',
               '-threads', '1', '-fflags', '+bitexact', '-flags:a', '+bitexact', audio)
        ffmpeg('-f', 'rawvideo', '-pixel_format', 'rgb24', '-video_size',
               f'{WIDTH}x{HEIGHT}', '-framerate', FPS, '-i', raw, '-i', wav,
               '-map', '0:v:0', '-map', '1:a:0', '-t', VIDEO_SECONDS,
               '-af', 'afade=t=out:st=8:d=1', '-map_metadata', '-1',
               '-c:v', 'mpeg1video', '-pix_fmt', 'yuv420p', '-q:v', '4',
               '-g', '12', '-bf', '2', '-sc_threshold', '0',
               '-c:a', 'mp2', '-b:a', '96k', '-ar', RATE, '-ac', '2',
               '-threads', '1', '-fflags', '+bitexact',
               '-flags:v', '+bitexact', '-flags:a', '+bitexact', '-f', 'mpeg', video)
    reports = [verify(audio, False), verify(video, True)]
    assert sum(item['bytes'] for item in reports) < 1024 * 1024
    print(json.dumps(reports, indent=2))


if __name__ == '__main__':
    main()
