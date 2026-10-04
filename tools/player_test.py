"""Exercise the actual player client with SB16 playback and capture its UI."""
import argparse
import pathlib
import tempfile
from mp3_test import make_fixture,guest_capture
if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('build',type=pathlib.Path)
    args=parser.parse_args()
    directory=pathlib.Path(tempfile.mkdtemp(prefix='baseos-player-'))
    print(f'Player UI evidence: {directory}',flush=True)
    make_fixture(directory,44100,2,'96k')
    guest_capture(args.build.resolve(),directory,'player_guest.c','PLAYER-UI-PASS','PLAYER-SCREEN')
    assert (directory/'player.png').exists()
    print('Player controls, playlist, pause/resume, volume and screenshot verified.')
