"""Focused host-only preparation/oracle/observation checks; never starts QEMU."""
from array import array
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock
import wave

import numpy as np
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import native_gui_media_input_test as gate
from test_native_window_collector import logical_pointer
from snapshot_responsiveness_test import COLORS, FIELDS


def peer_canvas(values):
    image=np.zeros((100,160,3),dtype=np.uint8)
    for block,color in enumerate(COLORS): image[:8,block*8:(block+1)*8]=color
    for row,value in enumerate(values):
        for bit in range(32):
            if value & (1<<bit): image[12+row*10:17+row*10,bit*5:bit*5+4]=255
    return image


class GuiMediaCollectorTests(unittest.TestCase):
    def test_profiles_are_mandatory_baseline_and_large(self):
        self.assertEqual(gate.PROFILES,{'default':64,'large':128})
        self.assertEqual(gate.AUDIO_SECONDS,45)
        self.assertEqual(gate.SAVE_TIMEOUT,180)

    def test_no_slot_never_starts_guest(self):
        with mock.patch.object(gate,'Session',side_effect=AssertionError('guest created')):
            with self.assertRaisesRegex(AssertionError,'exclusive QEMU slot'):
                gate.run(Path('/unread'),Path('/uncreated'),None)

    def test_memory_and_debug_routes_are_not_allowed(self):
        session=gate.Session.__new__(gate.Session)
        with self.assertRaises(AssertionError): session.memory(0,1)
        for name in ('pmemsave','human-monitor-command','query-blockstats'):
            with self.assertRaisesRegex(AssertionError,'Disallowed'):
                session.command(name,{})

    def test_held_file_revalidation(self):
        with tempfile.TemporaryDirectory() as temporary:
            path=Path(temporary)/'held';path.write_bytes(b'held original input')
            record={'input':gate.file_record(path)}
            gate.verify_records(record)
            path.write_bytes(b'different host input')
            with self.assertRaisesRegex(AssertionError,'Held input changed'):
                gate.verify_records(record)

    def test_pair_decodes_peer_and_real_native_viewport(self):
        font=gate.NativeFont()
        screen=np.full((720,1280,3),32,dtype=np.uint8)
        values=(1,2,300,9876,2000,3,0,0)
        peer=peer_canvas(values).repeat(2,0).repeat(2,1)
        screen[83:283,15:335]=peer
        window=gate.arranged_window(screen,'right')
        x,y,w,h=gate.native_viewport(window)
        from native_window_input_test import resample
        screen[y:y+h,x:x+w]=resample(logical_pointer(font,(160,100)),w,h)
        session=gate.Session.__new__(gate.Session);session.canvas=None
        session.directory=Path('/unwritten');session.frame=lambda keep:(screen,123.5)
        both=gate.read_pair(session,font,'host-only')
        self.assertEqual(both['probe'],dict(zip(FIELDS,values)))
        self.assertEqual(both['pointer']['done'],8)
        self.assertEqual(both['pointer']['sequence'],42)
        self.assertEqual(both['wall'],123.5)

    def test_player_exact_source_font_position(self):
        font=gate.ShellFont()
        for state in ('Playing','Finished'):
            screen=np.full((720,1280,3),232,dtype=np.uint8)
            x,y,w,h=gate.arranged_window(screen)
            width=sum(font.advance[ord(c)-32] for c in state)
            left,top=x+1+w-2-30-width,y+33+56
            mask=font.bitmap(state,'ui')
            screen[top:top+18,left:left+mask.shape[1]][mask]=30
            session=mock.Mock();session.frame.return_value=(screen,123.5);session.directory=Path('/unwritten')
            self.assertEqual(gate.player_state(session,font,state,'host-only')['state'],state)

    def test_every_sample_oracle_accepts_only_documented_phase_and_rounding(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder=Path(temporary);reference=folder/'reference.s16';capture=folder/'capture.wav'
            expected=array('h',[0,0]*11+[value for i in range(1024) for value in (2000+i,-2500-i)])
            reference.write_bytes(expected.tobytes())
            observed=array('h',[0,0]*37+[v+1 if v else 0 for v in expected]+[0,0]*17)
            with wave.open(str(capture),'wb') as output:
                output.setnchannels(2);output.setsampwidth(2);output.setframerate(44100)
                output.writeframes(observed.tobytes())
            result=gate.verify_audio(capture,reference)
            self.assertEqual(result['leading_frames'],37)
            self.assertEqual(result['samples'],len(expected))
            self.assertEqual(result['peak'],1)
            self.assertTrue(result['leading_silence_verified'])
            self.assertLessEqual(result['rms'],1)

    def test_prepare_failure_retains_unrun_status_without_guest(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder=Path(temporary)
            with mock.patch.object(gate,'Session',side_effect=AssertionError('guest created')):
                with self.assertRaises(FileNotFoundError):
                    gate.prepare(folder/'missing-build',folder/'evidence',folder/'missing-log')
            result=json.loads((folder/'evidence/manifest.json').read_text())
            self.assertEqual(result['status'],'PREPARATION FAILED')
            self.assertTrue(result['preparation_only'])
            self.assertFalse(result['runner_guest_qualified'])
            self.assertEqual(set(result['lanes'].values()),{'NOT RUN'})

if __name__=='__main__':unittest.main()
