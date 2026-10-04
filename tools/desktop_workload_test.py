"""Real multi-app workload: Writer, native stats, file copy, HTTP and SB16 MP3.

Only normal production PS/2 actions, disposable disks and a loopback HTTP fixture
are used. The captured complete audio stream is compared sample-by-sample with
the independently executed host decoder.
"""
import argparse
import http.server
import json
import pathlib
import subprocess
import tempfile
import threading
import time

from browser_download_input_test import type_url
from docstats_input_test import TerminalCheck
from download_input_test import download_status
from file_clipboard_input_test import FilesCheck
from mp3_test import host_decode
from mpeg_av_test import verify_capture
from volume import load, resolve, commit
from writer_input_test import WriterSession, fixture, native, decode_native

ROOT = pathlib.Path(__file__).resolve().parents[1]
PAYLOAD = bytes((i * 37 + 11) & 255 for i in range(131072))


class Server(http.server.BaseHTTPRequestHandler):
    paths = []
    def log_message(self, *_):
        pass
    def do_GET(self):
        self.paths.append(self.path)
        self.send_response(200)
        self.send_header('Content-Length', str(len(PAYLOAD)))
        self.send_header('Content-Type', 'application/octet-stream')
        self.send_header('Connection', 'close')
        self.end_headers()
        try:
            for start in range(0, len(PAYLOAD), 1024):
                self.wfile.write(PAYLOAD[start:start + 1024]); self.wfile.flush(); time.sleep(.05)
        except (BrokenPipeError, ConnectionResetError):
            pass


def run(build):
    work = pathlib.Path(tempfile.mkdtemp(prefix='baseos-desktop-workload-data-'))
    print(work, flush=True)
    song = work / 'work.mp3'
    subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y', '-stream_loop', '3',
                    '-i', str(ROOT / 'assets/examples/harbor.mp3'), '-t', '60',
                    '-codec:a', 'libmp3lame', '-b:a', '80k', '-write_xing', '0', str(song)], check=True)
    reference = host_decode(song, work)
    bulk = (b'alpha beta\n' * 190651)[:2097152]
    assert len(bulk) == 2097152
    writer_text = b'alpha line\n' * 1000
    disk = fixture(work, [('bulk.txt', bulk), ('stats-path.txt', b'/Documents/bulk.txt\n'),
                          ('work.bwr', native(writer_text)), ('work.mp3', song.read_bytes())])
    data = disk.read_bytes(); slot, generation, nodes = load(data)
    nodes[6] = dict(parent=0, name='Target', directory=1, app=0, data=b'', modified=0)
    commit(disk, data, slot, generation, nodes)
    capture = work / 'capture.wav'
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Server)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    events = []; screenshots = []
    try:
        with WriterSession(build, 'desktop-workload', extra=[
            '-drive', f'file={disk},format=raw,index=0,if=ide', '-nic', 'user,model=rtl8139',
            '-audiodev', f'wav,id=out,path={capture},out.frequency=44100,out.channels=2,out.format=s16',
            '-device', 'sb16,audiodev=out']) as session:
            print(session.directory, flush=True)
            session.boot(); files = FilesCheck(session, build); terminal = TerminalCheck(session, build)
            ui = files.ui
            session.launch('files'); files_slot = files.front()['slot']
            files.folder(disk, '/Documents'); files.select(2); files.clipboard('ctrl-c')
            session.launch('work.bwr'); writer_slot = files.front()['slot']
            session.key('ctrl-h'); session.text('alpha'); session.key('tab'); session.text('beta')
            session.launch('terminal'); native_slot = files.front()['slot']
            session.text('start /Programs/docstats.bex'); session.key('ret')
            session.wait(lambda: terminal.ready(native_slot), 'initial complete 2 MiB native scan', 45)
            session.launch('browser'); browser_slot = files.front()['slot']
            type_url(session, f'http://10.0.2.2:{server.server_port}/combined.bin')
            session.launch('media player')
            for _ in range(5):
                session.key('equal')
            session.launch('work.mp3'); session.wait(lambda: ui.o.audio()['state'] == 2, 'MP3 playback')
            assert ui.o.audio()['volume'] == 100
            started = time.monotonic()
            def event(name):
                audio = ui.o.audio()
                assert audio['error'] == 0 and audio['underruns'] == 0, audio
                events.append(dict(name=name, seconds=round(time.monotonic()-started, 3),
                                   audio_state=audio['state'], played_frames=audio['played_frames'],
                                   redraws=ui.o.integer('redraw_count')))
                (work / 'progress.json').write_text(json.dumps(events, indent=2) + '\n')
                print(json.dumps(events[-1]), flush=True)
            event('audio started')
            files.focus(browser_slot); session.key('ctrl-d')
            session.wait(lambda: download_status(session)['state'] == 1 and download_status(session)['received'] > 0,
                         'real HTTP download in progress')
            files.focus(native_slot); session.key('r')
            event('native rescan and HTTP active')
            files.focus(files_slot); files.folder(disk, '/Target'); files.clipboard('ctrl-v')
            nodes = load(disk.read_bytes())[2]
            assert nodes[files.state()['selected_id']]['data'] == bulk
            event('2 MiB copy synchronized')
            files.focus(writer_slot); session.key('ctrl-shift-ret')
            expected_writer = writer_text.replace(b'alpha', b'beta')
            session.wait(lambda: ui.content()['text'] == expected_writer,
                         'Writer Replace All completed after input dispatch', 20)
            session.key('esc'); session.key('ctrl-s')
            def writer_saved():
                n = load(disk.read_bytes())[2]
                return decode_native(n[resolve(n, '/Documents/work.bwr')]['data'])['text'] == expected_writer
            session.wait(writer_saved, 'Writer replacements durably saved', 45)
            event('Writer replaced and synchronized 1000 matches')
            files.focus(native_slot); session.key('s')
            report_path = f'/Documents/stats-{native_slot + 1}.txt'
            def report_ready():
                try:
                    n = load(disk.read_bytes())[2]
                    report = n[resolve(n, report_path)]['data']
                    return (f'Bytes: {len(bulk)}\n'.encode() in report and
                            f'Words: {len(bulk.split())}\n'.encode() in report and
                            f'Lines: {len(bulk.splitlines())}\n'.encode() in report)
                except ValueError:
                    return False
            session.wait(report_ready, 'concurrent native scan report', 45)
            event('native report synchronized')
            session.wait(lambda: download_status(session)['state'] in (2, 3, 4), 'HTTP completes', 30)
            status = download_status(session)
            assert status['state'] == 2 and status['received'] == len(PAYLOAD), status
            nodes = load(disk.read_bytes())[2]
            assert nodes[status['file_id']]['data'] == PAYLOAD
            assert Server.paths == ['/combined.bin']
            event('complete HTTP bytes synchronized')
            assert all(item['audio_state'] == 2 for item in events), events
            files.focus(writer_slot); session.key('alt-left')
            files.focus(native_slot); session.key('alt-right')
            screenshots.append(str(session.screenshot('writer-native-http-audio-workload.png')))
            session.wait(lambda: ui.o.audio()['state'] in (4, 5), 'full MP3 ends', 90)
            audio = ui.o.audio()
            assert audio['state'] == 4 and audio['error'] == 0 and audio['underruns'] == 0, audio
            assert audio['played_frames'] == audio['total_frames'], audio
            time.sleep(.3)
            event('every audio frame finished')
        verify_capture(capture, reference, 44100, 44100)
    finally:
        server.shutdown(); server.server_close()
    result = {'passed': True, 'events': events, 'screenshots': screenshots,
              'http_bytes': len(PAYLOAD), 'copied_bytes': len(bulk), 'writer_replacements': 1000,
              'audio': audio, 'checks': ['normal concurrent apps', 'exact large copy and durable Writer edits',
                                       'complete native scan/report', 'real HTTP bytes', 'all captured MP3 samples',
                                       'zero SB16 underruns throughout']}
    (work / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=pathlib.Path)
    run(parser.parse_args().build.resolve())
