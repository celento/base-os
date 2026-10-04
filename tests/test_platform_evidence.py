"""Host checks of ordinary production-platform evidence collection.

No QEMU execution, runtime mutation, fault injection or application fuzzing.
"""
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

import numpy as np

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from platform_evidence import COLORS, MAGIC, PlatformSession, decode_canvas, fnv, signed, snapshot_jobs
from platform_foundation_test import counter_canvas, fixture, frozen_apps, hello_canvas, operation_key
from volume import data_layout, load, resolve


def drawn_canvas(values,scale=1):
    pixels=np.full((720,1280,3),33,dtype=np.uint8);x,y=71,53
    pixels[y:y+200*scale,x:x+320*scale]=0
    for block,color in enumerate(COLORS):
        pixels[y:y+8*scale,x+block*8*scale:x+(block+1)*8*scale]=color
    for row,value in enumerate(values):
        for bit in range(32):
            if value&(1<<bit):
                bx,by=x+bit*5*scale,y+(12+row*10)*scale
                pixels[by:by+5*scale,bx:bx+4*scale]=255
    return pixels,(x,y,scale)


class PlatformEvidenceTests(unittest.TestCase):
    def test_canvas_published_words_and_signed_errors_at_both_scales(self):
        values=[MAGIC,0x10000003,2,0,7,700,1234,(-1006)&0xffffffff,0x20000005,
                2,8192,fnv(b'A'*4096),1,0x30000004,85,(-1005)&0xffffffff,0,0]
        for scale in (1,2):
            pixels,where=drawn_canvas(values,scale)
            result,canvas=decode_canvas(pixels)
            self.assertEqual(canvas,where)
            self.assertEqual(result['file_result'],-1006)
            self.assertEqual(result['foreign_result'],-1005)
            self.assertEqual(result['operation'],0x30000004)
            self.assertEqual(result['keys'],7)
            pixels[where[1]+14*scale,where[0]+2*scale]=(30,31,32)
            self.assertEqual(decode_canvas(pixels),(None,None))

    def test_capability_pages_are_not_interpreted_as_file_status(self):
        for page in (1,2,3):
            values=[MAGIC,7,2,page,1,50,70]+list(range(11))
            pixels,_=drawn_canvas(values)
            result,_=decode_canvas(pixels)
            self.assertNotIn('file_result',result)
            self.assertEqual(result['page'],page)
        self.assertEqual(signed(0xffffffff),-1)
        self.assertEqual(signed(0),0)

    def test_serial_completion_pairs_generation_and_timer_wrap(self):
        jobs=snapshot_jobs('FS snapshot begin tick=FFFFFFFA generation=00000009\n'
                           'FS snapshot end tick=00000004 generation=00000009 result=durable\n'
                           'FS snapshot begin tick=00000008 generation=0000000A\n')
        self.assertEqual(jobs[0]['result'],'durable')
        self.assertAlmostEqual(jobs[0]['seconds'],10/70)
        self.assertNotIn('end_tick',jobs[1])
        with self.assertRaises(ValueError):
            snapshot_jobs('FS snapshot end tick=00000004 generation=00000009 result=durable\n')

    def test_collector_forbids_guest_memory_and_monitor_commands(self):
        session=PlatformSession.__new__(PlatformSession)
        for method,args in ((session.memory,(0,1)),(session.command,('pmemsave',{})),
                            (session.command,('human-monitor-command',{})),
                            (session.command,('system_reset',{}))):
            with self.assertRaises(AssertionError):method(*args)

    def test_frozen_artifacts_retain_preplatform_identity(self):
        apps,manifest=frozen_apps()
        self.assertEqual(manifest['source_revision'],'a7ea36be6db5ae2cd477dfe36e8e58db48145048')
        self.assertEqual(set(apps),{'hello.bex','hello-c.bex','notebook.bex','counter.bex','docstats.bex'})
        for data in apps.values():
            magic,entry,length,reserved=struct.unpack_from('<4I',data)
            self.assertEqual(magic,0x31584542)
            self.assertEqual(length,len(data));self.assertEqual(reserved,0)
            self.assertGreaterEqual(entry,16)

    def test_operation_gate_requires_fresh_keyboard_consumption(self):
        class Session:
            def __init__(self):self.calls=0
            def key(self,key):self.key_sent=key
            def until(self,predicate,_message,**_kwargs):
                before=dict(process=3,page=0,keys=9,file_result=-1006)
                self.calls+=1
                if self.calls==1:
                    self.assert_before=predicate(before)
                    return before,None,0
                assert not predicate(before), 'A previous equal error must not satisfy a new action'
                assert not predicate(dict(before,process=4,keys=10))
                after=dict(before,keys=10)
                assert predicate(after)
                return after,None,1
        session=Session()
        observed=operation_key(session,'x',lambda o:o['file_result']==-1006,'conflict')
        self.assertEqual(observed['keys'],10)
        self.assertEqual(session.key_sent,'x')

    def test_focus_requires_target_owner_to_consume_harmless_ack(self):
        class Session:
            focus=PlatformSession.focus
            def __init__(self):self.phase=0;self.sent=[]
            def observe(self):
                # The first displayed target frame is stale; the first echo
                # actually reaches another owner. Never accept it as focus.
                value=dict(process=8 if self.phase==1 else 7,keys=5 if self.phase==3 else 4)
                return value,None,0
            def key(self,key,delay=0):
                self.sent.append(key)
                if key=='ctrl-tab':self.phase=2
                elif self.phase==0:self.phase=1
                elif self.phase==2:self.phase=3
        session=Session()
        clock=iter(i*.5 for i in range(100))
        with patch('platform_evidence.time.monotonic',side_effect=lambda:next(clock)),\
             patch('platform_evidence.time.sleep'):
            observed=session.focus(7)
        self.assertEqual(observed,dict(process=7,keys=5))
        self.assertEqual(session.sent,['a','ctrl-tab','a'])

    def test_fixture_profiles_use_exact_frozen_apps_and_manifest(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory=Path(temporary);app=directory/'platform.bex';app.write_bytes(b'ordinary fixture app')
            frozen,_=frozen_apps()
            for profile in ('default','large'):
                disk,expected,details=fixture(directory/profile,profile,app,bulk=False)
                nodes=load(disk.read_bytes())[2]
                self.assertEqual(nodes,expected)
                self.assertEqual(details['payload_bytes'],4096)
                self.assertLess(details['snapshot_bytes'],data_layout(profile).payload_limit)
                self.assertEqual(nodes[resolve(nodes,'/Documents/platform.bin')]['data'],b'A'*8192)
                manifest=struct.unpack('<12I',nodes[resolve(nodes,'/Documents/platform-manifest.bin')]['data'])
                self.assertEqual(manifest[:4],(MAGIC,1,4096,fnv(bytes(range(256))*16)))
                for name,data in frozen.items():
                    self.assertEqual(nodes[resolve(nodes,'/Programs/'+name)]['data'],data)

    def test_unchanged_counter_and_hello_display_decoders(self):
        shapes=((7,5,5,5,7),(2,6,2,2,7),(7,1,7,4,7),(7,1,7,1,7),(5,5,7,1,1),
                (7,4,7,1,7),(7,4,7,5,7),(7,1,1,1,1),(7,5,7,5,7),(7,5,7,1,7))
        for scale in (1,2):
            canvas=np.zeros((100,160,3),dtype=np.uint8)
            canvas[4:8,4:156]=(63,163,91)
            for i,n in enumerate((0,1,2,3)):
                for row in range(5):
                    for col in range(3):
                        if shapes[n][row]&(1<<(2-col)):
                            canvas[22+row*10:31+row*10,12+i*37+col*8:19+i*37+col*8]=(42,167,200)
            pixels=np.full((720,1280,3),33,dtype=np.uint8)
            pixels[50:50+100*scale,60:60+160*scale]=np.repeat(np.repeat(canvas,scale,0),scale,1)
            self.assertEqual(counter_canvas(pixels)['value'],123)
            levels=(0,64,128,192,255)
            for y in range(0,100,10):
                for x in range(0,160,10):
                    index=(x//10+y//10*5)%125
                    canvas[y:y+10,x:x+10]=(levels[index//25],levels[(index//5)%5],levels[index%5])
            pixels[50:50+100*scale,60:60+160*scale]=np.repeat(np.repeat(canvas,scale,0),scale,1)
            self.assertEqual(hello_canvas(pixels),[60,50,scale])


if __name__=='__main__':unittest.main()
