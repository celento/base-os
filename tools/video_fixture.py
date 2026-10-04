"""Generate an original deterministic MPEG-1 program stream and YUV reference.

The source is drawn locally rather than downloaded. Every frame has different
luma/chroma, a moving square, and a frame-index barcode; I/P/B pictures exercise
presentation order and inter-picture prediction. FFmpeg is used only to encode
the fixture and to provide an independent decoder reference.
"""
import argparse
from fractions import Fraction
import json
import pathlib
import subprocess


def run_ffmpeg(arguments):
    subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y',
                    *map(str, arguments)], check=True)


def source_frame(width, height, index):
    """One full-range-distinct, legal limited-range YUV420 test picture."""
    assert width > 0 and height > 0 and width % 2 == height % 2 == 0
    y = bytearray(width * height)
    square = min(width, height) // 5
    left = (index * 7) % (width - square)
    top = (index * 3) % (height - square)
    for row in range(height):
        # Smooth diagonals with discontinuities also exercise nonzero AC terms.
        line = bytearray(32 + ((column * 2 + row + index * 3) % 184)
                         for column in range(width))
        if top <= row < top + square:
            line[left:left + square] = bytes([224 - index % 17]) * square
        if row < 12:
            # Wide cells remain distinguishable after lossy MPEG encoding.
            for bit in range(8):
                start = bit * width // 8
                end = (bit + 1) * width // 8
                line[start:end] = bytes([224 if index & (1 << bit) else 24]) * (end - start)
        y[row * width:(row + 1) * width] = line
    planes = [y]
    for plane in range(2):
        chroma = bytearray(width * height // 4)
        for row in range(height // 2):
            line = bytes(48 + ((column * (3 + plane) + row * (2 - plane)
                               + index * (5 + 2 * plane) + plane * 73) % 160)
                         for column in range(width // 2))
            start = row * (width // 2)
            chroma[start:start + width // 2] = line
        planes.append(chroma)
    return b''.join(planes)


def make_fixture(directory, width=320, height=240, fps='25', frames=75,
                 name='sample', bframes=2, gop=12, codec='mpeg1video', audio=False):
    """Create a MPEG-1 program stream and return its path (no external assets)."""
    directory = pathlib.Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    original = directory / (name + '.source.yuv')
    with original.open('wb') as output:
        for index in range(frames):
            output.write(source_frame(width, height, index))
    encoded = directory / (name + '.mpg')
    audio_input = (['-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=32000']
                   if audio else [])
    audio_output = (['-c:a', 'mp2', '-b:a', '64k', '-shortest'] if audio else ['-an'])
    run_ffmpeg(['-f', 'rawvideo', '-pixel_format', 'yuv420p', '-video_size',
                f'{width}x{height}', '-framerate', fps, '-i', original,
                *audio_input, '-frames:v', frames, *audio_output, '-c:v', codec, '-q:v', '3',
                '-g', gop, '-bf', bframes, '-sc_threshold', '0', '-threads', '1',
                '-flags', '+bitexact', '-fflags', '+bitexact', '-f', 'mpeg', encoded])
    return encoded


def decode_reference(encoded, output=None):
    """Decode visible pixels in presentation order as planar Y, Cb, Cr."""
    encoded = pathlib.Path(encoded)
    output = pathlib.Path(output) if output else encoded.with_suffix('.reference.yuv')
    run_ffmpeg(['-threads', '1', '-i', encoded, '-map', '0:v:0', '-an',
                '-vsync', '0', '-pix_fmt', 'yuv420p', '-f', 'rawvideo', output])
    return output


def probe_fixture(encoded):
    probe = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v:0',
                            '-show_streams', '-show_frames', '-of', 'json',
                            str(encoded)], check=True, capture_output=True, text=True)
    metadata = json.loads(probe.stdout)
    stream = metadata['streams'][0]
    frame_rate = stream['avg_frame_rate']
    if frame_rate == '0/0':  # A single picture has no measured average interval.
        frame = metadata['frames'][0]
        duration = frame.get('duration') or frame['pkt_duration']
        frame_rate = 1 / (int(duration) * Fraction(stream['time_base']))
    return {'codec': stream['codec_name'], 'width': stream['width'],
            'height': stream['height'], 'fps': str(Fraction(frame_rate)),
            'frames': len(metadata['frames']),
            'picture_types': [frame['pict_type'] for frame in metadata['frames']]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=pathlib.Path)
    parser.add_argument('--width', type=int, default=320)
    parser.add_argument('--height', type=int, default=240)
    parser.add_argument('--fps', default='25')
    parser.add_argument('--frames', type=int, default=75)
    parser.add_argument('--name', default='sample')
    args = parser.parse_args()
    encoded = make_fixture(args.directory, args.width, args.height, args.fps,
                           args.frames, args.name)
    reference = decode_reference(encoded)
    metadata = probe_fixture(encoded)
    metadata.update(encoded=str(encoded), reference=str(reference),
                    bytes=encoded.stat().st_size)
    report = encoded.with_suffix('.json')
    report.write_text(json.dumps(metadata, indent=2) + '\n')
    print(json.dumps(metadata, indent=2))


if __name__ == '__main__':
    main()
