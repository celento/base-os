"""Production platform vertical with ordinary BEX1 clients and real PS/2 input.

Uses the supplied clean build without rebuilding it. The caller must hold the
serialized QEMU slot. Raw disks/screenshots stay in --work outside Git; only the
compact JSON/report and at most two representative PNGs should be archived.
"""
import argparse
import json
from pathlib import Path
import shutil
import struct
import time
import traceback

import numpy as np

from build_app import build as build_app
from init_data import initialize
from platform_evidence import (ROOT, MAGIC, PlatformSession, fnv, provenance, sha, snapshot_jobs)
from volume import data_layout, encode_snapshot, load, resolve

NOTE = b'This note was saved by a protected C application.\n'
STATS = b'alpha beta\ngamma delta\n'
STALE, CHANGED, BUSY = -1005, -1006, -1004


def frozen_apps():
    folder = ROOT / 'tests/fixtures/bex1-legacy'
    manifest = json.loads((folder / 'manifest.json').read_text())
    result = {}
    for name, expected in manifest['files'].items():
        data = (folder / name).read_bytes()
        assert len(data) == expected['bytes'] and sha(data) == expected['sha256'], name
        result[name] = data
    return result, manifest


def fixture(directory, profile, app, seed=None, bulk=True):
    directory = Path(directory);directory.mkdir(parents=True, exist_ok=True)
    disk = directory / (profile + '.img')
    assert initialize(disk, profile=profile)
    layout = data_layout(profile)
    nodes = load(Path(seed).read_bytes())[2] if seed else {
        0: dict(parent=-1, name='', directory=1, app=0, data=b'', modified=0),
        1: dict(parent=0, name='Documents', directory=1, app=0, data=b'', modified=0),
        2: dict(parent=0, name='Programs', directory=1, app=0, data=b'', modified=0)}
    documents, programs = resolve(nodes, '/Documents'), resolve(nodes, '/Programs')
    lengths = ([2 << 20, 2 << 20, 2 << 20, 1 << 20] if profile == 'default' else [16 << 20, 12 << 20]) if bulk else [4096]
    payloads = [bytes(range(256)) * (length // 256) for length in lengths]
    manifest = [MAGIC, len(payloads)]
    for data in payloads:
        manifest += [len(data), fnv(data)]
    manifest += [0] * (12 - len(manifest))
    old, _ = frozen_apps()
    entries = [(programs, name, data) for name, data in old.items()]
    entries += [(programs, 'platform.bex', Path(app).read_bytes()),
                (documents, 'platform.bin', b'A' * 8192),
                (documents, 'platform-manifest.bin', struct.pack('<12I', *manifest)),
                (documents, 'stats-sample.txt', STATS),
                (documents, 'stats-path.txt', b'/Documents/stats-sample.txt\n')]
    entries += [(documents, f'payload{i}.bin', data) for i, data in enumerate(payloads)]
    for parent, name, content in entries:
        matches = [i for i, node in nodes.items() if node['parent'] == parent and node['name'] == name]
        ident = matches[0] if matches else next(i for i in range(layout.node_limit) if i not in nodes)
        nodes[ident] = dict(parent=parent, name=name, directory=0, app=0, data=content, modified=123400 + ident)
    header, payload = encode_snapshot(nodes, layout, 1)
    raw = bytearray(disk.read_bytes()); offset = layout.lbas[0] * 512
    raw[offset:offset + 512] = header.ljust(512, b'\0')
    raw[offset + 512:offset + 512 + len(payload)] = payload
    assert load(raw)[2] == nodes
    disk.write_bytes(raw)
    return disk, nodes, dict(payload_bytes=sum(lengths), snapshot_bytes=len(payload), lengths=lengths)


def qemu_args(disk, profile):
    return ['-m', '128M' if profile == 'large' else '64M', '-drive',
            f'file={disk},format=raw,index=0,if=ide,cache=writeback']


def quiet(session, seconds=180):
    started = time.monotonic()
    def done():
        serial = session.log.read_text()
        return time.monotonic() - started > 8 and not any('end_tick' not in job for job in snapshot_jobs(serial))
    session.wait(done, 'all ordinary boot/session snapshots finished', seconds)
    assert 'result=error' not in session.log.read_text()


def durable_contains(disk, path, expected):
    try:
        nodes = load(disk.read_bytes())[2]
        return nodes[resolve(nodes, path)]['data'] == expected
    except ValueError:
        return False


def counter_canvas(pixels):
    """Decode the unchanged old counter's ordinary four-digit canvas."""
    digits = ((7,5,5,5,7),(2,6,2,2,7),(7,1,7,4,7),(7,1,7,1,7),(5,5,7,1,1),
              (7,4,7,1,7),(7,4,7,5,7),(7,1,1,1,1),(7,5,7,5,7),(7,5,7,1,7))
    for bar in ((63,163,91),(242,201,76)):
        ys,xs = np.where(np.all(pixels == bar, axis=2))
        for py,px in zip(ys,xs):
            if px and tuple(pixels[py,px-1]) == bar:
                continue
            if py and tuple(pixels[py-1,px]) == bar:
                continue
            for scale in (1,2):
                x,y=int(px-4*scale),int(py-4*scale)
                if x<0 or y<0 or x+160*scale>pixels.shape[1] or y+100*scale>pixels.shape[0]:
                    continue
                if not np.all(pixels[py,px:px+152*scale] == bar):
                    continue
                result=0
                for position in range(4):
                    values=[]
                    for row in range(5):
                        bits=0
                        for col in range(3):
                            color=tuple(pixels[y+(22+row*10+4)*scale,x+(12+position*37+col*8+3)*scale])
                            if color in ((42,167,200),(242,201,76)):
                                bits |= 1 << (2-col)
                            elif color != (0,0,0):
                                bits=-100
                        values.append(bits)
                    if tuple(values) not in digits:
                        break
                    result=result*10+digits.index(tuple(values))
                else:
                    return dict(value=result,paused=bar==(242,201,76),canvas=[x,y,scale])
    return None


def hello_canvas(pixels):
    levels=(0,64,128,192,255)
    expected=np.zeros((100,160,3),dtype=np.uint8)
    for y in range(0,100,10):
        for x in range(0,160,10):
            index=(x//10+y//10*5)%125
            expected[y:y+10,x:x+10]=(levels[index//25],levels[(index//5)%5],levels[index%5])
    # The unique first transition from black to blue is a compact locator.
    ys,xs=np.where(np.all(pixels == (0,0,64),axis=2))
    for py,px in zip(ys,xs):
        if px and tuple(pixels[py,px-1])==(0,0,64):continue
        if py and tuple(pixels[py-1,px])==(0,0,64):continue
        for scale in (1,2):
            x,y=int(px-10*scale),int(py)
            if x<0 or y+100*scale>pixels.shape[0] or x+160*scale>pixels.shape[1]:continue
            target=np.repeat(np.repeat(expected,scale,0),scale,1)
            if np.array_equal(pixels[y:y+100*scale,x:x+160*scale],target):return [x,y,scale]
    return None


def compatibility(build, work, app):
    disk, original, _ = fixture(work, 'default', app, bulk=False)
    _, manifest = frozen_apps()
    result=dict(frozen=manifest, evidence=[], checks=[])
    with PlatformSession(build, 'platform-compatibility', extra=qemu_args(disk,'default')) as session:
        result['directory']=str(session.directory);session.boot();quiet(session)
        session.launch('terminal');session.key('alt-ret')
        for mode in ('exec','start'):
            session.text(mode+' /Programs/hello-c.bex');session.key('ret')
            session.wait(lambda:hello_canvas(session.frame()[0]) is not None,'unchanged C hello canvas in '+mode)
            result['checks'].append('frozen hello-c '+mode+' exact canvas')
        hello_position=hello_canvas(session.frame()[0]);assert hello_position
        session.text('exec /Programs/hello.bex');session.key('ret')
        def assembly_canvas():
            pixels,_=session.frame();x,y,scale=hello_position
            canvas=pixels[y:y+100*scale,x:x+160*scale]
            mask=np.any(canvas!=0,axis=2);expected=np.zeros((100*scale,160*scale),dtype=bool)
            expected[20*scale:21*scale,20*scale:21*scale]=True
            return np.array_equal(mask,expected)
        session.wait(assembly_canvas,'unchanged assembly hello exact plotted point')
        result['checks'].append('frozen assembly hello legacy exec exact canvas')
        session.text('exec /Programs/notebook.bex');session.key('ret')
        session.wait(lambda:durable_contains(disk,'/Documents/sdk-note.txt',NOTE),'legacy notebook durable bytes',90)
        quiet(session)
        result['checks'].append('frozen notebook legacy exec exact durable note')
        session.text('start /Programs/docstats.bex');session.key('ret');time.sleep(1)
        session.key('s')
        def stats_saved():
            nodes=load(disk.read_bytes())[2]
            for node in nodes.values():
                if node['name'].startswith('stats-') and node['name'].endswith('.txt') and node['data'].startswith(b'Document: '):
                    expected=(f'Document: /Documents/stats-sample.txt\nBytes: {len(STATS)}\n'
                              f'Words: {len(STATS.split())}\nLines: {len(STATS.splitlines())}\nByte sum: {sum(STATS)}\n').encode()
                    if node['data']==expected:return True
            return False
        session.wait(stats_saved,'frozen docstats streamed exact durable report',90)
        quiet(session);session.key('q');session.text('start /Programs/counter.bex');session.key('ret')
        first=None
        def counter_ready():
            nonlocal first
            first=counter_canvas(session.frame()[0]);return first is not None
        session.wait(counter_ready,'frozen counter initial canvas')
        session.wait(lambda:(o:=counter_canvas(session.frame()[0])) and o['value']!=first['value'],'frozen counter progresses')
        session.key('spc');time.sleep(.2);paused=counter_canvas(session.frame()[0]);assert paused and paused['paused']
        session.key('equal');time.sleep(.2);changed=counter_canvas(session.frame('compatibility')[0]);assert changed['value']==(paused['value']+10)%10000
        result['checks']+=['frozen docstats exact durable report','frozen counter independent progress and PS/2 +10']
        session.key('s')
        session.wait(lambda:durable_contains(disk,'/Documents/counter-1.txt',(str(changed['value'])+'\n').encode()),'frozen counter saves exact paused value',90)
        quiet(session)
        session.key('q');session.key('ctrl-w');quiet(session)
        assert 'PANIC:' not in session.log.read_text()
    # Cold process replacement is an actual boot from saved media, never a
    # RAM/disk reload inside a running guest. It also supplies a seeded disk.
    with PlatformSession(build,'platform-compatibility-reboot',extra=qemu_args(disk,'default')) as reboot:
        reboot.boot();assert 'FS loaded from disk' in reboot.log.read_text()
        assert durable_contains(disk,'/Documents/sdk-note.txt',NOTE)
        quiet(reboot)
        result['reboot_directory']=str(reboot.directory)
    result['disk_sha256']=sha(disk.read_bytes());result['passed']=True
    return disk,result


def operation_key(session, key, predicate, message, seconds=120):
    before=session.until(lambda o:o['page']==0,'native client before '+key)[0]
    session.key(key)
    return session.until(lambda o:o['process']==before['process'] and
                         o['keys']>before['keys'] and predicate(o),message,seconds=seconds)[0]


def enter_handle(session,handle,message):
    before=session.until(lambda o:o['page']==0,'client before owner-handle check')[0]
    text='h'+f'{handle:08x}'
    session.text(text);session.key('ret')
    return session.until(lambda o:o['process']==before['process'] and
                         o['keys']>=before['keys']+len(text)+1 and o['foreign_result']==STALE,message)[0]


def run_profile(build,work,profile,app,seed,timeout,staged_default=False):
    assert not staged_default or profile=="default"
    disk,original,details=fixture(work,profile,app,seed)
    result=dict(profile=profile,ram_mib=128 if profile=='large' else 64,**details,checks=[],responses=[],
                workload='staged-default-fresh-save' if staged_default else 'original-operation-sequence')
    with PlatformSession(build,'platform-'+profile,extra=qemu_args(disk,profile)) as session:
        result['directory']=str(session.directory)
        print(profile+' evidence: '+str(session.directory),flush=True)
        session.boot();quiet(session,timeout)
        for _ in range(16):
            session.command('input-send-event',{'events':[
                {'type':'rel','data':{'axis':'x','value':-80}},
                {'type':'rel','data':{'axis':'y','value':-80}}]})
            time.sleep(.01)
        session.launch('terminal');a=session.start_client();aid=a['process']
        caps=session.page(1);limits=session.page(2);session.page(0)
        assert caps['major']==1 and caps['minor']>=0 and caps['struct_size']==96
        assert (caps['features']&15)==15 and caps['context']==2 and caps['user_bytes']==65536 and caps['image_bytes']==49152
        assert caps['stack_bytes']==16384 and caps['chunk_bytes']==4096 and caps['replace_bytes']==32768
        assert caps['file_bytes']==data_layout(profile).file_limit
        assert limits['hz']==70 and limits['wait_ms']==60000 and limits['operations_per_process']>0
        result.update(capabilities=caps,limits=limits);result['checks'].append('truthful bounded native capabilities')
        a=operation_key(session,'o',lambda o:o['file_result']==4096,'client A first-version read')
        assert a['read_hash']==fnv(b'A'*4096)
        b=session.start_client();bid=b['process'];assert aid!=bid and a['slot']!=b['slot']
        b=operation_key(session,'o',lambda o:o['file_result']==4096,'client B same-version read')
        assert b['read_hash']==a['read_hash']
        result['concurrent_readers']=dict(first=a,second=b)
        # Retry only the documented transient write-lease outcome.
        deadline=time.monotonic()+timeout
        while True:
            b=operation_key(session,'w',lambda o:o['file_result'] in (0,BUSY),'ordinary conditional write result')
            if b['file_result']==0:break
            assert time.monotonic()<deadline,'write lease never released';time.sleep(.3)
        newer=b['revision'];assert newer!=a['revision']
        result['newer_writer']=b
        session.focus(aid)
        a=operation_key(session,'r',lambda o:o['file_result']==CHANGED,'reader gets explicit CHANGED')
        assert a['read_hash']==fnv(b'A'*4096),'changed read must not replace accepted chunk'
        result['reader_changed']=a
        a=operation_key(session,'x',lambda o:o['file_result']==CHANGED,'stale writer gets explicit CHANGED')
        result['writer_conflict']=a
        result['checks'].append('two ordinary readers/writers reject changed chunks and lost update')
        print(profile+' explicit changed read and conditional conflict passed',flush=True)
        # Reopen admits the current B version, then request its durability.
        a=operation_key(session,'o',lambda o:o['file_result']==4096,'reader reopens current version')
        assert a['read_hash']==fnv(b'B'*4096)
        a=operation_key(session,'s',lambda o:o['sync_result']==1 and o['operation']!=0,'real async save is pending')
        handle_a=a['operation'];result['first_pending']=a
        session.focus(bid)
        b=operation_key(session,'s',lambda o:o['sync_result']==1 and o['operation']!=0,'second owner joins pending durability')
        assert b['operation']!=handle_a;handle_b=b['operation'];result['second_pending']=b
        b=enter_handle(session,handle_a,'other owner cannot consume first receipt')
        assert b['sync_result']==1,'ownership checks must occur during a real pending commit'
        result['nonowner_result']=b
        session.focus(aid);session.key('l')
        a=session.until(lambda o:o['operation']==0 and o['sync_result']==0,'release only first owner receipt')[0]
        a=operation_key(session,'s',lambda o:o['sync_result']==1 and o['operation']!=0,'owner requests another receipt before normal close')
        abandoned=a['operation'];assert abandoned!=handle_a
        result['abandoned_pending']=a
        session.key('ctrl-w')
        session.focus(bid)
        b=session.until(lambda o:o['operation']==handle_b and o['sync_result']==1,'release/close leaves second save active')[0]
        c=session.start_client();cid=c['process'];assert cid not in (aid,bid)
        assert c['slot']==a['slot'],'ordinary closed Terminal slot was not reused'
        c=enter_handle(session,abandoned,'reused slot cannot own old completion')
        result['reused_owner_result']=c
        result['checks'].append('owned receipts, release, close and reused-slot stale cleanup')
        print(profile+' owner-bound pending receipts, release, close and slot reuse passed',flush=True)
        active_id,active_handle,active_start=bid,handle_b,b
        # The frozen Counter is a third, independently scheduled old binary.
        session.key('ctrl-n');session.text('start /Programs/counter.bex');session.key('ret');session.key('alt-ret')
        counter=None
        def ready():
            nonlocal counter
            counter=counter_canvas(session.frame()[0]);return counter is not None
        session.wait(ready,'old independent counter visible during pending save')
        if staged_default:
            # The optimized default save may already be terminal. Preserve that
            # historical receipt and create a fresh ordinary save in owner C,
            # with the Counter now installed/running before input measurement.
            session.focus(bid)
            first_done=session.until(lambda o:o['operation']==handle_b and o['sync_result']==0,
                                     'first owner durable before staged save',seconds=timeout)[0]
            result['completion']=first_done
            result['completed_client_limits']=session.page(2);session.page(0)
            session.focus(cid)
            c=operation_key(session,'o',lambda o:o['file_result']==4096,'new owner opens current B version')
            assert c['read_hash']==fnv(b'B'*4096)
            deadline=time.monotonic()+timeout
            while True:
                c=operation_key(session,'w',lambda o:o['file_result'] in (0,BUSY),'fresh same-content conditional write')
                if c['file_result']==0:break
                assert time.monotonic()<deadline,'fresh write lease never released';time.sleep(.3)
            c=operation_key(session,'s',lambda o:o['sync_result']==1 and o['operation']!=0,
                            'fresh owned save is genuinely pending')
            active_id,active_handle,active_start=cid,c['operation'],c
            result['fresh_pending']=c
            for _ in range(9):
                counter=counter_canvas(session.frame()[0])
                if counter:break
                session.key('ctrl-tab',delay=.5)
            else:raise AssertionError('running Counter could not be focused for staged save')
        before=counter;sent=session.key('equal',delay=0);deadline=time.monotonic()+12
        while time.monotonic()<deadline:
            pixels,wall=session.frame();counter=counter_canvas(pixels)
            if counter and counter['value']>=before['value']+10:break
            time.sleep(.01)
        else:raise AssertionError('counter did not respond to real PS/2 during save')
        assert any('end_tick' not in job for job in snapshot_jobs(session.log.read_text())), 'counter response was outside pending save'
        result['counter_response']=dict(before=before,after=counter,input_to_visible_ms=(wall-sent)*1000)
        session.focus(active_id)
        operation_key(session,'t',lambda o:o['sync_result']==1,'bounded wait returns to its pending owner')
        waited=session.page(3)
        assert waited['wait_result']==-1010 and waited['wait_ticks']>=2 and waited['operation']==active_handle
        assert waited['sync_result']==1
        result['bounded_wait']=waited;session.page(0)
        before_mouse,before_pixels,_=session.observe()
        assert before_mouse['sync_result']==1
        sent_mouse=time.monotonic()
        session.command('input-send-event',{'events':[
            {'type':'rel','data':{'axis':'x','value':80}},
            {'type':'rel','data':{'axis':'y','value':80}}]})
        deadline=time.monotonic()+12
        while time.monotonic()<deadline:
            after_mouse,after_pixels,wall=session.observe()
            changed=np.any(after_pixels!=before_pixels,axis=2)
            old_count=int(changed[:24,:24].sum());new_count=int(changed[75:112,75:112].sum())
            if old_count>3 and new_count>3:break
            time.sleep(.01)
        else:raise AssertionError('normal PS/2 pointer movement did not become visible during save')
        assert after_mouse and after_mouse['sync_result']==1
        result['mouse_response']=dict(input_to_visible_ms=(wall-sent_mouse)*1000,old_pixels=old_count,new_pixels=new_count)
        for i in range(3):
            prior=session.until(lambda o:o['page']==0,'owner visible')[0]
            if prior['sync_result']!=1:break
            sent=session.key('a',delay=0)
            after,_,wall=session.until(lambda o:o['keys']>prior['keys'],'native input response during save',seconds=12)
            result['responses'].append(dict(before=prior,after=after,input_to_visible_ms=(wall-sent)*1000))
        assert result['responses'] and result['responses'][0]['after']['sync_result']==1
        completed=session.until(lambda o:o['operation']==active_handle and o['sync_result']==0,
            'active owner receives durable completion',seconds=timeout,keep='durable')[0]
        assert completed['pending']>active_start['pending'] and completed['loops']>active_start['loops']
        print(profile+' measured responsive pending save completed durably',flush=True)
        if staged_default:
            result['new_owner_completion']=completed
        else:
            result['completion']=completed
            result['completed_client_limits']=session.page(2);session.page(0)
            session.focus(cid)
            c=operation_key(session,'s',lambda o:o['operation']!=0 and o['sync_result'] in (0,1),'new owner can save while another retains result')
            c=session.until(lambda o:o['sync_result']==0,'new owner completion',seconds=timeout)[0]
            result['new_owner_completion']=c
        result['checks'].append('real pending save preserves counter and ordinary PS/2 progress')
        print(profile+' new owner completed while prior owner retained its durable receipt',flush=True)
        session.focus(bid)
        assert session.until(lambda o:o['operation']==handle_b,'old owner still has own result')[0]['sync_result']==0
        quiet(session,timeout)
        serial=session.log.read_text();assert 'PANIC:' not in serial and 'result=error' not in serial
        result['snapshot_jobs']=snapshot_jobs(serial)
        result['checks'].append('retained completion does not starve subsequent owner')
    nodes=load(disk.read_bytes())[2]
    assert nodes[resolve(nodes,'/Documents/platform.bin')]['data']==b'B'*8192
    for ident,node in original.items():
        if node['name'] in ('platform.bin','session'):continue
        assert nodes[ident]['data']==node['data'],('unrelated persisted bytes changed',node['name'])
    result['disk_before_reboot_sha256']=sha(disk.read_bytes())
    with PlatformSession(build,'platform-'+profile+'-reboot',extra=qemu_args(disk,profile)) as reboot:
        result['reboot_directory']=str(reboot.directory);reboot.boot()
        assert 'FS loaded from disk' in reboot.log.read_text()
        reboot.launch('terminal');reboot.start_client();reboot.key('v')
        verified=reboot.until(lambda o:o['verified']==MAGIC and o['errors']==0,
                              'actual reboot reads exact saved B and hashes every bulk file',seconds=timeout,keep='reboot-verified')[0]
        result['reboot_verified']=verified;quiet(reboot,timeout)
    assert durable_contains(disk,'/Documents/platform.bin',b'B'*8192)
    result['disk_after_reboot_sha256']=sha(disk.read_bytes());result['passed']=True
    result['checks'].append('actual reboot public-API full-payload verification and independent disk decode')
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build',type=Path)
    parser.add_argument('--work',type=Path,required=True)
    parser.add_argument('--profile',choices=('default','large','both'),default='both')
    parser.add_argument('--seed',type=Path,help='Prior compatibility seed, skips compatibility boots')
    parser.add_argument('--compatibility-result',type=Path,
                        help='Explicitly reuse a passed compatibility result from this exact candidate; requires --seed')
    parser.add_argument('--staged-default',action='store_true',help='Use a fresh same-content owned save after Counter setup on default only')
    parser.add_argument('--old-build',type=Path,help='Optional clean preplatform build for actual query fallback boot')
    parser.add_argument('--save-timeout',type=int,default=900,
                        help='Bound a complete large-volume snapshot under concurrent native rendering')
    args=parser.parse_args();args.work.mkdir(parents=True,exist_ok=True)
    result=dict(passed=False,provenance=provenance(args.build),profiles=[],observation_scope='Only ordinary SDK apps, PS/2, published canvas, serial and stopped-disk decode; no guest memory, debugger, injected calls or faults')
    for name in ('platform_foundation_test.py','platform_evidence.py','platform_context_test.py'):
        shutil.copy2(ROOT/'tools'/name,args.work/name)
    shutil.copy2(ROOT/'tests/platform_client_app.c',args.work/'platform_client_app.c')
    result['harness_files']={name:sha((args.work/name).read_bytes()) for name in
                             ('platform_foundation_test.py','platform_evidence.py',
                              'platform_context_test.py','platform_client_app.c')}
    try:
        app=args.work/'platform.bex';build_app(ROOT/'tests/platform_client_app.c',app)
        result['app_sha256']=sha(app.read_bytes())
        if args.seed:
            seed=args.seed
            if args.compatibility_result:
                prior=json.loads(args.compatibility_result.read_text())
                assert prior['compatibility']['passed']
                for name in ('boot.bin','kernel.bin','kernel.packed'):
                    assert prior['provenance']['artifacts'][name]['sha256']==result['provenance']['artifacts'][name]['sha256']
                assert sha(seed.read_bytes())==prior['seed_sha256']==prior['compatibility']['disk_sha256']
                result['compatibility']=prior['compatibility']
                result['compatibility_reused']=dict(result_path=str(args.compatibility_result),
                    result_sha256=sha(args.compatibility_result.read_bytes()),
                    built_source=prior['provenance']['built_source'],seed_sha256=prior['seed_sha256'])
        else:seed,result['compatibility']=compatibility(args.build,args.work/'compatibility',app)
        result['seed_sha256']=sha(seed.read_bytes())
        from platform_context_test import run_contexts
        result['contexts']=run_contexts(args.build,args.work/'contexts',app,args.old_build)
        profiles=('default','large') if args.profile=='both' else (args.profile,)
        for profile in profiles:
            result['profiles'].append(run_profile(args.build,args.work/profile,profile,app,seed,args.save_timeout,args.staged_default and profile=='default'))
            (args.work/'result.json').write_text(json.dumps(result,indent=2)+'\n')
        result['passed']=True
    except BaseException as error:
        result['failure']=dict(type=type(error).__name__,message=str(error),traceback=traceback.format_exc())
        raise
    finally:
        (args.work/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print('PASS: '+str(args.work/'result.json'),flush=True)


if __name__=='__main__':main()
