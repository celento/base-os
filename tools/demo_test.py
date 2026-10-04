"""Play every frame/sample of the original longer examples in the normal desktop."""
import argparse
import json
import pathlib
import shutil
import struct
import subprocess
import tempfile
import time
from demo_data import install
from init_data import initialize
from layout import constants
from update_image import install_kernel
from qemu_session import DesktopSession
from mp3_test import host_decode
from mpeg_av_fixture import probe_av_fixture
from mpeg_av_test import host_reference, verify_capture

ROOT = pathlib.Path(__file__).resolve().parents[1]


def run(build):
    directory = pathlib.Path(tempfile.mkdtemp(prefix='baseos-demo-playback-'))
    print('Original demo evidence:', directory, flush=True)
    c = constants()
    disk = bytearray(c['DISK_SECTORS'] * 512)
    disk[:512] = (build / 'boot.bin').read_bytes()
    install_kernel(disk, (build / 'kernel.bin').read_bytes(), c)
    boot = directory / 'baseos.img'; boot.write_bytes(disk)
    data = directory / 'baseos-data.img'; initialize(data)
    install(build, boot, data, 1791072000)
    symbols = [line.split() for line in subprocess.check_output(['nm', '-S', str(build / 'kernel.elf')], text=True).splitlines()]
    audio_address = next(int(p[0], 16) for p in symbols if len(p) == 4 and p[3] == 'status' and int(p[1], 16) == 48)
    video_address = next(int(p[0], 16) for p in symbols if len(p) == 4 and p[3] == 'status' and int(p[1], 16) == 84)
    results = []
    for suffix in ('mp3', 'mpg'):
        work = directory / suffix; work.mkdir()
        source = work / ('harbor.' + suffix)
        shutil.copyfile(ROOT / 'assets/examples' / source.name, source)
        if suffix == 'mp3':
            reference = host_decode(source, work)
        else:
            metadata = probe_av_fixture(source)
            reference = host_reference(source, work, metadata)
        recording = work / 'capture.wav'
        extra = ['-drive', f'file={data},format=raw,index=0,if=ide', '-nic', 'none',
                 '-audiodev', f'wav,id=out,path={recording},out.frequency=44100,out.channels=2,out.format=s16',
                 '-device', 'sb16,audiodev=out']
        with DesktopSession(build, 'demo-' + suffix, image=boot, extra=extra) as session:
            session.boot(); session.launch('media player')
            for _ in range(5): session.key('equal')
            def audio(): return struct.unpack('<12I', session.memory(audio_address, 48))
            def video(): return struct.unpack('<21I', session.memory(video_address, 84))
            assert audio()[10] == 100, audio()
            session.launch(source.name)
            session.wait(lambda: audio()[0] == 2, 'Demo audio did not start')
            if suffix == 'mpg':
                assert video()[0] == 2 and video()[14] == 1, video()
                assert video()[2:6] == (320, 240, 25, 1), video()
            time.sleep(2)
            screenshot = session.screenshot('harbor-' + suffix + '-playing.png')
            session.wait(lambda: audio()[0] in (4, 5), 'Demo audio did not finish', 40)
            assert audio()[0] == 4 and audio()[1] == 0 and audio()[11] == 0, audio()
            if suffix == 'mpg':
                session.wait(lambda: video()[0] in (4, 5), 'Demo video did not finish')
                assert video()[0] == 4 and video()[1] == 0 and video()[8:10] == (225, 225), video()
            state = {'audio': list(audio()), 'video': list(video()) if suffix == 'mpg' else None}
            time.sleep(.3)
            session.screenshot('harbor-' + suffix + '-finished.png')
            state.update(file=source.name, screenshot=str(screenshot), guest_directory=str(session.directory))
            results.append(state)
        verify_capture(recording, reference, 44100, 44100)
    (directory / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    print('Production desktop: full 18-second MP3 and 225-frame MPEG/MP2 examples passed with zero underruns.', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=pathlib.Path)
    run(parser.parse_args().build.resolve())
