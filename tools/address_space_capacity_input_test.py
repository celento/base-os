"""Visible ordinary BEX2 capacity refusal, close/retry and survivor checks.

Prepare first; execution requires the coordinator's exclusive QEMU slot. All
programs are valid; there is no forced allocator limit, invalid access, debugger,
memory reader, monitor command or live-disk read. Uses normal PS/2 and screenshots.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import sys

import address_space_input_test as desktop

ROOT=Path(__file__).resolve().parents[1]
CAPACITY_TEXT='Cannot start: native backing memory is unavailable.'


def footprint(data):
    h=struct.unpack_from('<16I',data)
    assert h[0]==0x32584542
    return (h[6]+4095)//4096+(h[9]+4095)//4096+h[10]//4096+h[11]//4096+2


def source_identity():
    return {str(p.relative_to(ROOT)):desktop.artifact(p) for p in
            (Path(__file__),ROOT/'tests/address_space_hold_app.c')}


def prepare(args,runtime,modules):
    work=args.work
    if work.exists() and any(work.iterdir()):raise ValueError('Need new empty work directory')
    work.mkdir(parents=True,exist_ok=True)
    report=dict(passed=False,status='prepared',provenance=desktop.source_provenance(args,runtime,modules),
                capacity_sources=source_identity(),profiles={},plans={})
    assert CAPACITY_TEXT in (runtime/'src/term.c').read_text(),'Review new production capacity wording'
    frozen=ROOT/'tests/fixtures/bex1-legacy'
    manifest=json.loads((frozen/'manifest.json').read_text());counter=frozen/'counter.bex'
    assert desktop.artifact(counter)==manifest['files']['counter.bex']
    apps={'counter.bex':counter.read_bytes()}
    for name,pages in (('hold-small',256),('hold-large',460)):
        app=work/(name+'.bex')
        command=[sys.executable,str(runtime/'tools/build_app.py'),str(ROOT/'tests/address_space_hold_app.c'),
                 str(app),'--format','bex2','--workspace-bytes',str(pages*4096),
                 '--stack-bytes','65536','--elf-output',str(work/(name+'.elf'))]
        result=subprocess.run(command,capture_output=True,text=True)
        (work/(name+'.log')).write_text(result.stdout+result.stderr)
        result.check_returncode()
        apps[app.name]=app.read_bytes();report['plans'][name]=dict(workspace_bytes=pages*4096,
            owned_pages=footprint(apps[app.name]),command=command,artifact=desktop.artifact(app))
    small=report['plans']['hold-small']['owned_pages'];large=report['plans']['hold-large']['owned_pages']
    report['capacity_arithmetic']=dict(rejected=small*2+large+16,admitted=small+large+16)
    assert small*2+large+16>1021 and small+large+16<=797
    report['original_files']={}
    for name,data in apps.items():
        path=work/name
        if not path.exists():path.write_bytes(data)
        report['original_files']['/Programs/'+name]=desktop.artifact(path)
    constants=modules['layout'].constants();boot=bytearray(constants['DISK_SECTORS']*512)
    boot[:512]=(args.build/'boot.bin').read_bytes()
    modules['update_image'].install_kernel(boot,(args.build/'kernel.bin').read_bytes(),constants)
    (work/'boot.img').write_bytes(boot)
    for profile in ('default','large'):
        disk=work/(profile+'.img');desktop.seed_disk(disk,profile,apps,{},modules)
        report['profiles'][profile]=dict(ram_mib=64 if profile=='default' else 128,initial_disk=desktop.artifact(disk))
    report['prepared_files']={p.name:desktop.artifact(p) for p in sorted(work.iterdir()) if p.is_file()}
    (work/'report.json').write_text(json.dumps(report,indent=2)+'\n')


def ready(session,font,plan,label,timeout,only=None):
    ids=(only,) if only is not None else range(1,9)
    choices=[f"Ready workspace {plan['workspace_bytes']} task {i} owned {plan['owned_pages']}" for i in ids]
    found=desktop.wait_visible(session,font,[],label,timeout,choices)
    return int(found['alternative'].split()[4])


def run_profile(args,profile,report,modules,font):
    p=report['profiles'][profile];disk=args.work/(profile+'.img')
    Session=modules['platform_evidence'].PlatformSession
    extra=['-m',str(p['ram_mib'])+'M','-drive',f'file={disk},format=raw,index=0,if=ide,cache=writeback','-nic','none']
    small=report['plans']['hold-small'];large=report['plans']['hold-large']
    with Session(args.build,'bex2-capacity-'+profile,extra=extra,image=args.work/'boot.img') as session:
        p['directory']=str(session.directory);session.boot();session.launch('counter.bex')
        desktop.wait_counter(session,modules['platform_foundation_test'].counter_canvas,
                             lambda value:not value['paused'],'counter-live',args.timeout)
        session.launch('hold-small.bex');session.key('alt-ret')
        first=ready(session,font,small,'first-held',args.timeout)
        session.launch('hold-small.bex');session.key('alt-ret')
        second=ready(session,font,small,'second-held',args.timeout)
        assert first!=second
        session.launch('hold-large.bex');session.key('alt-ret')
        desktop.wait_visible(session,font,[CAPACITY_TEXT],'capacity-refused-visible',args.timeout)
        session.key('ctrl-w')
        ready(session,font,small,'second-before-close',args.timeout,second)
        session.key('ctrl-w')
        ready(session,font,small,'first-after-capacity-close',args.timeout,first)
        session.launch('hold-large.bex');session.key('alt-ret')
        replacement=ready(session,font,large,'same-request-now-ready-zero',args.timeout)
        assert replacement!=first
        session.key('ctrl-w')
        ready(session,font,small,'first-survived-larger-owner',args.timeout,first)
        session.key('v')
        desktop.wait_visible(session,font,[f'Verified survivor task {first}'],'survivor-verified',args.timeout)
        session.key('q');desktop.wait_visible(session,font,['Native task finished.'],'survivor-normal-exit',args.timeout)
        session.key('ctrl-w');session.key('q')
        desktop.wait_visible(session,font,['Native task finished.'],'counter-normal-exit',args.timeout)
        session.key('ctrl-w');desktop.shutdown(session,args.timeout)
        p.update(first_task=first,closed_task=second,replacement_task=replacement,
                 events=session.events,passed=True)
    p['preserved_files']=desktop.validate_stopped_files(disk,report,{},modules['volume'])
    p['final_disk']=desktop.artifact(disk)


def main(args):
    args.build=args.build.resolve();args.work=args.work.resolve()
    runtime,modules=desktop.runtime_tools(args.build)
    if args.prepare_only:
        prepare(args,runtime,modules);print('Prepared only; no QEMU. '+str(args.work/'report.json'));return
    if not args.qemu_slot_held:raise ValueError('Exclusive QEMU slot is required')
    report=json.loads((args.work/'report.json').read_text());assert report['status']=='prepared'
    assert source_identity()==report['capacity_sources']
    for name,expected in report['prepared_files'].items():assert desktop.artifact(args.work/name)==expected,name
    current=desktop.source_provenance(args,runtime,modules)
    for field in ('built_source','artifacts','runtime_source_sha256','runtime_helper_sha256','production_object_sha256','harness_sha256'):
        assert current[field]==report['provenance'][field],field
    report['execution_provenance']=current;report['status']='running'
    path=args.work/'report.json';path.write_text(json.dumps(report,indent=2)+'\n')
    try:
        font=desktop.VisibleText(runtime/'src/font.h')
        for profile in report['profiles']:
            run_profile(args,profile,report,modules,font);path.write_text(json.dumps(report,indent=2)+'\n')
        report.update(passed=True,status='passed')
    except BaseException as error:
        report.update(passed=False,status='failed',error=repr(error));raise
    finally:
        report['evidence_artifacts']={str(p):desktop.artifact(p) for item in report['profiles'].values()
            for p in (Path(item['directory']).iterdir() if item.get('directory') else []) if p.is_file()}
        path.write_text(json.dumps(report,indent=2)+'\n')
    print('PASS: visible natural capacity refusal, close/retry, zeroed larger workspace and intact survivor.')
    print(path)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('build',type=Path)
    parser.add_argument('--expected-revision',required=True);parser.add_argument('--build-log',type=Path,required=True)
    parser.add_argument('--work',type=Path,required=True);parser.add_argument('--timeout',type=int,default=120)
    mode=parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--prepare-only',action='store_true');mode.add_argument('--run-prepared',action='store_true')
    parser.add_argument('--qemu-slot-held',action='store_true');main(parser.parse_args())
