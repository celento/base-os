"""Production owned-window Monitor/capacity gate; requires an explicit QEMU slot.

Use a previously frozen native_window_input_test manifest. Own fresh volumes are
created, and only ordinary PS/2, screenshots/serial and stopped-volume checks
observe the guest. This is a separate gate; it never edits the prepared inputs.
"""
import argparse
import json
from pathlib import Path
from native_window_input_test import (Evidence, PROFILES, make_volume, phase_run,
                                     require, save_json, verify_inputs, stopped_volume)


def monitor_layout(window, font):
    x,y,w,h=window
    by=y+33
    left=x+16; width=w-32; gap=6; tab=(width-2*gap)//3
    advance=font.advance
    text=lambda s:sum(advance[ord(c)-32] for c in s)
    stop=text('Stop task')+32;show=text('Show window')+32;close=text('Close')+32
    return dict(tabs=[(left+i*(tab+gap)+tab//2,by+26) for i in range(3)],
                show=(left+width-stop-gap-show//2,by+85),
                stop=(left+width-stop//2,by+85),
                close=(left+width-close//2,by+90))


def shell_phase(session,evidence):
    session.boot();session.origin();session.move(1275,670)
    session.launch('window-document.bex');session.key('alt-ret')
    baseline=evidence.document('primary-before-monitor')
    session.launch('pointer-window.bex');session.key('alt-ret')
    state=evidence.pointer('peer-before-monitor')
    session.launch('Monitor');session.key('alt-ret')
    monitor=state['window'];controls=monitor_layout(monitor,evidence.shell)
    session.click(*controls['tabs'][2]);evidence.frame('monitor-two-native-tasks')
    # Tasks are enumerated by owning WM slot: document first, Pointer second.
    session.click(controls['stop'][0],controls['stop'][1]+42)
    evidence.frame('monitor-stop-peer')
    session.click(*controls['show']);session.key('m')
    restored=evidence.document('monitor-shows-owned-primary')
    evidence.check('Monitor Show restores real owned window; Stop frees peer pages',
                   restored['slot']==baseline['slot'] and restored['free']==baseline['free'],
                   baseline=baseline,after=restored)
    session.key('ctrl-m') # Return to Monitor, not a manufactured Terminal.
    session.click(*controls['tabs'][1]);evidence.frame('monitor-retained-result-window')
    session.click(controls['close'][0],controls['close'][1]+38)
    evidence.frame('monitor-closes-retained-result')
    # Two remaining windows: minimized document and Monitor. Fill all six free
    # slots with ordinary visible Terminals; no record/page pressure is faked.
    for _ in range(6):session.launch('Terminal')
    session.key('alt-ret');session.text('start /Programs/pointer-window.bex');session.key('ret')
    image=evidence.frame('capacity-refusal-keeps-calling-shell')
    region=(0,36,image.shape[1],image.shape[0]-80)
    evidence.check('full eight-window launch is refused in existing shell',
        evidence.shell.contains(image,region,'Cannot start: close a window to run this app.'))
    session.text('echo capacity shell intact');session.key('ret')
    image=evidence.frame('capacity-shell-still-responsive')
    evidence.check('failed launch preserves calling shell input ownership',
        evidence.shell.contains(image,region,'capacity shell intact'))
    session.key('ctrl-w');session.launch('pointer-window.bex');session.key('alt-ret')
    retry=evidence.pointer('capacity-retry-new-native-window')
    evidence.check('closing one real window permits a fresh owned launch',
                   retry['reset']==1 and retry['done']==0 and not retry['buttons'],state=retry)
    session.key('ctrl-w')
    doc=evidence.cycle(evidence.document,'primary-after-capacity-retry');session.key('m')
    doc=evidence.document('primary-final-owned-pages')
    evidence.check('capacity failure/retry/Close preserves original owner and page count',
                   doc['slot']==baseline['slot'] and doc['free']==baseline['free'] and doc['saved']==0,
                   before=baseline,after=doc)
    session.key('q')


def run(prepared,output,profiles):
    manifest=json.loads((prepared/'manifest.json').read_text());verify_inputs(manifest)
    output.mkdir(parents=True,exist_ok=False)
    report=dict(status='RUNNING',source_manifest=str(prepared/'manifest.json'),profiles={})
    save_json(output/'results.json',report)
    apps={name:Path(record['path']).read_bytes() for name,record in manifest['apps'].items()}
    try:
        for profile in profiles:
            folder=output/profile;folder.mkdir();disk=folder/'data.img'
            initial=make_volume(disk,profile,apps)
            record=dict(initial_volume=initial,passed=False);report['profiles'][profile]=record
            save_json(output/'results.json',report)
            result=phase_run(Path(manifest['build_directory']),disk,PROFILES[profile],folder,
                             'monitor-capacity',shell_phase,manifest)
            record['stopped_volume']=stopped_volume(disk,manifest,False)
            record['guest']=result;record['passed']=True;save_json(output/'results.json',report)
        report['status']='PASSED'
    except Exception as error:
        report['status']='FAILED; EVIDENCE RETAINED';report['failure']=repr(error);raise
    finally:save_json(output/'results.json',report)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prepared',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--profiles',nargs='+',choices=tuple(PROFILES),default=list(PROFILES))
    args=parser.parse_args();run(args.prepared.resolve(),args.output.resolve(),args.profiles)
