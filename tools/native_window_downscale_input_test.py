"""Ordinary owned-result resize and session exclusion, using held app bytes.

Freeze the published Pointer frame by Stop, then compare every downscaled pixel
through real WM resize gestures. Only screenshots, PS/2 and stopped-volume files
are inspected; no guest-memory or private runtime state is read.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import numpy as np
from native_window_input_test import (ROOT,PROFILES,arranged_window,native_viewport,
    reconstruct,resample,make_volume,phase_run,require,save_json,verify_inputs,file_record,
    load,resolve)


def resize(session,window,width,height):
    x,y,w,h=window
    session.move(x+w-3,y+h-3);session.button(True)
    session.move(x+width-3,y+height-3);session.button(False)
    session.move(1275,670)
    return x,y,width,height


def downscale_phase(session,evidence):
    session.boot();session.origin();session.move(1275,670)
    session.launch('pointer-window.bex');session.key('alt-ret');session.key('r');session.key('ctrl-c')
    session.move(1275,670);image=evidence.frame('stopped-320-reference')
    window=arranged_window(image);logical=(320,200)
    reference=reconstruct(image,native_viewport(window,logical),logical)
    state=evidence.font.pointer(image,window,logical)
    evidence.check('stopped reference is complete320x200',state['logical']==[320,200],state=state)
    for w,h in ((283,207),(240,180),(499,301)):
        window=resize(session,window,w,h)
        image=evidence.frame(f'result-{w}x{h}')
        x,y,vw,vh=native_viewport(window,logical)
        observed=image[y:y+vh,x:x+vw]
        expected=resample(reference,vw,vh)
        evidence.check(f'complete published frame matches{vw}x{vh}nearest-neighbor viewport',
            np.array_equal(observed,expected),window=window,viewport=[x,y,vw,vh],
            expected_sha256=hashlib.sha256(expected.tobytes()).hexdigest(),
            observed_sha256=hashlib.sha256(observed.tobytes()).hexdigest())
    # Leave the inert owned window open. Shutdown session save must explicitly
    # exclude its kind rather than restoring an orphan after the next boot.


def session_records(disk):
    _,_,nodes=load(disk.read_bytes());data=nodes[resolve(nodes,'/prefs/session')]['data']
    require(struct.unpack_from('<II',data)==(0x53534542,1),'Expected ordinary v1 session file')
    require((len(data)-8)%8==0,'Expected eight bounded session records')
    stride=(len(data)-8)//8
    require(stride>=36,'Truncated saved-window record')
    return [struct.unpack_from('<9i',data,8+i*stride) for i in range(8)]


def run(prepared,output,profiles):
    manifest=json.loads((prepared/'manifest.json').read_text());verify_inputs(manifest)
    output.mkdir(parents=True,exist_ok=False)
    source=file_record(Path(__file__));report=dict(status='RUNNING',collector=source,profiles={})
    save_json(output/'results.json',report)
    apps={n:Path(r['path']).read_bytes() for n,r in manifest['apps'].items()}
    try:
        for profile in profiles:
            folder=output/profile;folder.mkdir();disk=folder/'data.img';initial=make_volume(disk,profile,apps)
            result=phase_run(Path(manifest['build_directory']),disk,PROFILES[profile],folder,
                             'downscale',downscale_phase,manifest)
            records=session_records(disk)
            require(all(not r[0] for r in records),'Owned result was written as a persistent window')
            report['profiles'][profile]=dict(passed=True,guest=result,initial=initial,
                stopped_volume=file_record(disk),session_records=[list(r) for r in records],
                native_result_explicitly_excluded=True)
            save_json(output/'results.json',report)
        require(file_record(Path(__file__))==source,'Collector changed during guest gate')
        report['status']='PASSED'
    except Exception as error:
        report['status']='FAILED; EVIDENCE RETAINED';report['failure']=repr(error);raise
    finally:save_json(output/'results.json',report)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prepared',type=Path,required=True);parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--profiles',nargs='+',choices=tuple(PROFILES),default=list(PROFILES))
    args=parser.parse_args();run(args.prepared.resolve(),args.output.resolve(),args.profiles)
