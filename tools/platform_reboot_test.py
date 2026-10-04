"""Bounded ordinary cold-boot verification from a copied, stopped test disk.

This does not resume the ownership workload or wait for unrelated session-state
autosaves. It preserves the original attempt and requires actual guest reads to
accept its persisted document and every bulk payload before reporting success.
"""
import argparse
import json
from pathlib import Path
import shutil
import struct
import traceback

from platform_evidence import MAGIC, PlatformSession, fnv, provenance, sha
from volume import decode, load, resolve


def inspect(raw,app_sha):
    slot,generation,nodes=load(raw)
    assert sha(nodes[resolve(nodes,'/Programs/platform.bex')]['data'])==app_sha
    assert nodes[resolve(nodes,'/Documents/platform.bin')]['data']==b'B'*8192
    manifest=struct.unpack('<12I',nodes[resolve(nodes,'/Documents/platform-manifest.bin')]['data'])
    assert manifest[0]==MAGIC and manifest[1]==2
    payloads=[]
    for i in range(manifest[1]):
        data=nodes[resolve(nodes,f'/Documents/payload{i}.bin')]['data']
        assert len(data)==manifest[2+i*2] and data==bytes(range(256))*(len(data)//256)
        assert fnv(data)==manifest[3+i*2]
        payloads.append(dict(bytes=len(data),sha256=sha(data),fnv=manifest[3+i*2]))
    assert sum(p['bytes'] for p in payloads)==28<<20
    return dict(offline_selected_slot=slot,offline_selected_generation=generation,
                valid_slots=[dict(slot=i,generation=d[0]) for i in range(2)
                             if (d:=decode(raw,i)) is not None],payloads=payloads,
                shared_bytes=8192,shared_sha256=sha(b'B'*8192))


def run(build,original,app,work,seconds=300):
    build,original,app,work=map(Path,(build,original,app,work));work.mkdir(parents=True,exist_ok=True)
    identity=provenance(build)
    app_sha=sha(app.read_bytes());original_sha=sha(original.read_bytes())
    result=dict(passed=False,provenance=identity,source_disk=str(original),source_disk_sha256=original_sha,
                app=str(app),app_sha256=app_sha,
                boundary='Original attempt completed both owned saves, then timed out waiting for unrelated session autosave generation4. This is a separate ordinary cold boot; incomplete generation4 is not claimed durable.')
    copied=work/'large-copy.img'
    assert not copied.exists(),'Never overwrite a prior evidence disk'
    shutil.copy2(original,copied)
    assert sha(copied.read_bytes())==original_sha
    result['stopped_disk']=inspect(copied.read_bytes(),app_sha)
    for source in (Path(__file__),Path(__file__).with_name('platform_evidence.py')):
        shutil.copy2(source,work/source.name)
    try:
        with PlatformSession(build,'platform-large-reboot-separate',extra=[
                '-m','128M','-drive',f'file={copied},format=raw,index=0,if=ide,cache=writeback']) as session:
            result['directory']=str(session.directory);session.boot()
            assert 'FS loaded from disk' in session.log.read_text()
            session.launch('terminal');session.start_client();session.key('v')
            verified,_,_=session.until(lambda o:o['verified']==MAGIC and o['errors']==0,
                'cold-booted ordinary app verifies exact B and every28MiB payload byte',seconds=seconds,keep='reboot-verified')
            result.update(guest_verified=verified,passed=True)
            result['serial_at_verification']=session.log.read_text()
            result['verification_png']=str(session.directory/'reboot-verified.png')
            (work/'result.json').write_text(json.dumps(result,indent=2)+'\n')
            print('COLD-REBOOT-GATE-PASS: '+str(work/'result.json'),flush=True)
            assert 'PANIC:' not in result['serial_at_verification']
        result['after_stop']=inspect(copied.read_bytes(),app_sha)
        result['copy_after_stop_sha256']=sha(copied.read_bytes())
        assert sha(original.read_bytes())==original_sha,'Original stopped evidence was changed'
        result['original_unchanged']=True
    except BaseException as error:
        result['passed']=False
        result['failure']=dict(type=type(error).__name__,message=str(error),traceback=traceback.format_exc())
        raise
    finally:
        (work/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    return result


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build',type=Path);parser.add_argument('disk',type=Path);parser.add_argument('app',type=Path)
    parser.add_argument('--work',type=Path,required=True);parser.add_argument('--seconds',type=int,default=300)
    args=parser.parse_args();run(args.build,args.disk,args.app,args.work,args.seconds)
