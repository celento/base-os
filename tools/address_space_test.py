"""Normal valid BEX2 client/accounting gates on disposable default/large disks.

The supplied runtime must be a clean identified enabled build, already approved
for guest gates. This script does not change or rebuild production sources.
Fixture-kernel evidence is distinct from a production PS/2 desktop gate.
There is no debugger, QMP, socket, guest-memory observation or invalid app probe.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import time

ROOT=Path(__file__).resolve().parents[1]
FIELDS='total free allocated high_water backing mapped pt pd records'.split()
COMPLETE=b'BEX2 ordinary address spaces complete\n'
NOTE=b'This note was saved by a protected C application.\n'


def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def expected_output(display):
    return bytes((i*37+display*13+(i>>12)*17)&255 for i in range(4064,4064+32768))


def prepare(args,work):
    build=args.build.resolve();runtime=build.parent
    # Runtime utilities must resolve their own exact layout and SDK, rather than
    # importing a newer or older fixture checkout's runtime-affecting defaults.
    sys.path.insert(0,str(runtime/'tools'))
    from build_app import build as build_app,tool
    from platform_evidence import provenance
    from layout import constants
    from update_image import install_kernel
    identity=provenance(build)
    assert identity['built_source']['revision']==args.expected_revision
    compilation=args.build_log.read_text()
    assert '#define BASEOS_BEX2_ENABLED 1' in (runtime/'src/program.h').read_text(), 'Require clean source-enabled candidate'
    assert any('src/process.c' in line and ' -c ' in line for line in compilation.splitlines()), 'Need process object build provenance'
    identity['runtime_build_log']=dict(path=str(args.build_log),sha256=sha(args.build_log))
    frozen=json.loads((ROOT/'tests/fixtures/bex1-hour05/manifest.json').read_text())
    files={}
    for name,info in frozen['files'].items():
        source=ROOT/'tests/fixtures/bex1-hour05'/name
        assert source.stat().st_size==info['bytes'] and sha(source)==info['sha256']
        files[name]=source.read_bytes()
    for name,pages in (('address-space',256),('address-large',460)):
        app=work/(name+'.bex');elf=work/(name+'.elf')
        build_app(ROOT/'tests/address_space_app.c',app,format='bex2',workspace_bytes=pages*4096,
                  stack_bytes=65536,required_abi_minor=1,elf_output=elf)
        files[app.name]=app.read_bytes()
    for label,value in (('a',111),('b',222)):
        app=work/('fpu-'+label+'.bex')
        subprocess.run(['nasm','-f','bin','-DVALUE='+str(value),str(ROOT/'tests/task_fpu.asm'),'-o',str(app)],check=True)
        files[app.name]=app.read_bytes()
    guest=ROOT/'tests/address_space_guest.c'
    command=[tool('gcc'),'-std=gnu11','-Os','-g','-Wall','-Wextra','-ffreestanding','-m32',
             '-fno-pie','-fno-pic','-fno-stack-protector','-fno-builtin','-mno-sse','-mno-mmx',
             '-msoft-float','-I',str(runtime),'-I',str(runtime/'src'),
             '-I',str(build),'-c',str(guest),'-o',str(work/'kernel.o')]
    subprocess.run(command,check=True)
    objects=[p for p in sorted(build.glob('*.o')) if p.name!='kernel.o']
    subprocess.run([tool('ld'),'-T',str(build/'linker.ld'),'-nostdlib','-m','elf_i386',
                    '-z','noexecstack','-o',str(work/'kernel.elf'),str(work/'kernel.o'),*map(str,objects)],check=True)
    subprocess.run([tool('objcopy'),'-O','binary',str(work/'kernel.elf'),str(work/'kernel.bin')],check=True)
    c=constants();image=bytearray(c['DISK_SECTORS']*512);image[:512]=(build/'boot.bin').read_bytes()
    install_kernel(image,(work/'kernel.bin').read_bytes(),c);(work/'boot.img').write_bytes(image)
    identity.update(evidence_class='valid-app fixture kernel; unchanged enabled production runtime objects',
        fixture_revision=subprocess.check_output(['git','-C',str(ROOT),'rev-parse','HEAD'],text=True).strip(),
        fixture_status=subprocess.check_output(['git','-C',str(ROOT),'status','--short'],text=True),
        frozen_bex1=frozen,fixture_compile_command=command,
        fixture_sources={str(p.relative_to(ROOT)):sha(p) for p in
                         (guest,ROOT/'tests/address_space_app.c',ROOT/'tests/task_fpu.asm',Path(__file__))},
        fixture_artifacts={p.name:sha(p) for p in work.iterdir() if p.is_file()},
        production_object_sha256={p.name:sha(p) for p in objects})
    return files,identity


def disk(path,profile,files):
    from init_data import initialize
    import volume
    assert not path.exists();initialize(path,profile=profile)
    nodes={0:dict(parent=-1,name='',directory=1,app=0,data=b'',modified=0),
           1:dict(parent=0,name='Programs',directory=1,app=0,data=b'',modified=0),
           2:dict(parent=0,name='Documents',directory=1,app=0,data=b'',modified=0)}
    for name,data in files.items():nodes[len(nodes)]=dict(parent=1,name=name,directory=0,app=0,data=data,modified=1234)
    nodes[len(nodes)]=dict(parent=2,name='source.bin',directory=0,app=0,
                          data=bytes((i*29+7)&255 for i in range(8192)),modified=1234)
    layout=volume.data_layout(profile);header,payload=volume.encode_snapshot(nodes,layout,1)
    raw=bytearray(path.read_bytes());offset=layout.lbas[0]*512
    raw[offset:offset+512]=header.ljust(512,b'\0');raw[offset+512:offset+512+len(payload)]=payload
    assert volume.load(raw)[2]==nodes;path.write_bytes(raw)


def run(boot,data,work,label,memory,marker,timeout):
    log=work/(label+'.log');errors=work/(label+'.stderr');log.write_text('')
    command=['qemu-system-i386','-m',memory,'-vga','std','-boot','a',
        '-drive',f'file={boot},format=raw,index=0,if=floppy',
        '-drive',f'file={data},format=raw,index=0,if=ide,cache=writeback',
        '-serial',f'file:{log}','-display','none','-monitor','none','-no-reboot']
    started=time.monotonic()
    with errors.open('w') as stream:
        process=subprocess.Popen(command,stdout=subprocess.DEVNULL,stderr=stream)
        try:
            while marker not in log.read_text():
                text=log.read_text()
                if 'PANIC:' in text or process.poll() is not None or time.monotonic()-started>timeout:
                    raise AssertionError(f'{label}: {marker} missing\n{text}\n{errors.read_text()}')
                time.sleep(.1)
        finally:
            if process.poll() is None:process.terminate()
            try:process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill();process.wait(timeout=5);raise AssertionError('QEMU did not close normally')
    text=log.read_text();assert 'PANIC:' not in text
    rows=[]
    for match in re.finditer(r'AS-COUNT ([\w-]+)\b'+''.join(r' '+field+r'=(\d+)' for field in FIELDS),text):
        rows.append(dict(label=match[1],**dict(zip(FIELDS,map(int,match.groups()[1:])))))
    assert rows,'No page-accounting records'
    assert rows[0]['allocated']==rows[-1]['allocated']==0
    latency=re.search(r'AS-LATENCY ticks_hz=(\d+) launch=(\d+) close=(\d+) slice=(\d+)',text)
    return dict(command=command,seconds=round(time.monotonic()-started,3),checkpoints=rows,
                latency=dict(zip(('hz','launch','close','slice'),map(int,latency.groups()))) if latency else None,
                log_sha256=sha(log),stderr_sha256=sha(errors))


def validate_files(path):
    import volume
    nodes=volume.load(path.read_bytes())[2]
    expected={'/Documents/as-complete.txt':COMPLETE,'/Documents/sdk-note.txt':NOTE,
              '/Documents/as-2.bin':expected_output(2),'/Documents/as-6.bin':expected_output(6)}
    for name,data in expected.items():assert nodes[volume.resolve(nodes,name)]['data']==data,name
    return {name:dict(bytes=len(data),sha256=hashlib.sha256(data).hexdigest()) for name,data in expected.items()}


def main(args):
    work=args.work.resolve();assert not work.exists() or not any(work.iterdir());work.mkdir(parents=True,exist_ok=True)
    report=dict(passed=False,prepare_only=args.prepare_only,profiles={})
    try:
        files,report['provenance']=prepare(args,work)
        for profile in (('default','large') if args.profile=='both' else (args.profile,)):
            data=work/(profile+'.img');disk(data,profile,files)
            item=report['profiles'][profile]=dict(ram_mib=64 if profile=='default' else 128,initial_disk_sha256=sha(data))
            if args.prepare_only:continue
            item['first']=run(work/'boot.img',data,work,profile,str(item['ram_mib'])+'M','ADDRESS-SPACE-FUNCTIONAL-PASS',args.timeout)
            item['before_reboot_files']=validate_files(data)
            item['reboot']=run(work/'boot.img',data,work,profile+'-reboot',str(item['ram_mib'])+'M','ADDRESS-SPACE-REBOOT-PASS',args.timeout)
            item['after_reboot_files']=validate_files(data)
            assert item['before_reboot_files']==item['after_reboot_files'];item['final_disk_sha256']=sha(data)
        report['passed']=not args.prepare_only
    finally:(work/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print('Prepared only; no guest started.' if args.prepare_only else 'PASS: ordinary BEX2 isolation, natural capacity, mixed legacy, owned cleanup and exact reboot files.')
    print(work/'report.json')


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('build',type=Path)
    parser.add_argument('--expected-revision',required=True);parser.add_argument('--build-log',type=Path,required=True)
    parser.add_argument('--work',type=Path,required=True);parser.add_argument('--profile',choices=('default','large','both'),default='both')
    parser.add_argument('--prepare-only',action='store_true');parser.add_argument('--timeout',type=int,default=180)
    main(parser.parse_args())
