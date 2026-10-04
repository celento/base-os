"""Create an original short PCM melody that fits a BaseOS filesystem file."""
import argparse
import math
import pathlib
import wave

def make_example(path):
    rate=11025
    notes=(523.251,659.255,783.991,1046.502)
    samples=bytearray()
    for note in notes:
        for i in range(rate//4):
            position=i/(rate//4)
            envelope=min(1.0,position*25)*max(0.0,1-position)**2
            sample=math.sin(2*math.pi*note*i/rate)*envelope
            samples.append(max(0,min(255,round(128+sample*60))))
    with wave.open(str(path),'wb') as output:
        output.setnchannels(1);output.setsampwidth(1);output.setframerate(rate)
        output.writeframes(samples)
    assert path.stat().st_size<16384
    print(f'{path}: {path.stat().st_size} bytes, original 8-bit mono PCM melody')

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output',type=pathlib.Path,nargs='?',default=pathlib.Path('build/chime.wav'))
    make_example(parser.parse_args().output)
