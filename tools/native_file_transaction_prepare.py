"""Prepare immutable ordinary file-transaction media; this tool never starts QEMU.

Old executables are copied only from hash-verified archives. New app binaries
must already exist in a clean frozen build. Live observation and execution need
separate admission; --verify-stopped only reads an exclusively locked disk.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
from init_data import initialize
from kernel_pack import unpack_kernel
from layout import constants
from volume import data_layout, encode_snapshot, load, locked_image, resolve

ROOT=Path(__file__).resolve().parents[1]

def digest(data):
    return hashlib.sha256(data).hexdigest()

def record(path):
    path=Path(path).resolve();data=path.read_bytes()
    return dict(path=str(path),bytes=len(data),sha256=digest(data))

def require(condition,message):
    if not condition:raise ValueError(message)

def document_bytes(seed=1,length=262144):
    require(0<=seed<=0xffffffff and 0<=length<=262144,'Unsupported document model')
    return bytes(10 if i%80==79 else 65+((i//80+i%80+seed)&0xffffffff)%26 for i in range(length))

def verified_archive(path,entry):
    result=record(path)
    require(all(result[key]==entry[key] for key in ('bytes','sha256')),f'Archive identity changed: {path}')
    return result

def verify_build(build):
    c=constants();image=(build/'baseos.img').read_bytes();boot=(build/'boot.bin').read_bytes()
    packed=(build/'kernel.packed').read_bytes();raw=(build/'kernel.bin').read_bytes()
    first=c['KERNEL_PRIMARY_SECTORS']*512;tail=c['KERNEL_EXT_LBA']*512
    installed=image[512:512+min(first,len(packed))]
    if len(packed)>first:installed+=image[tail:tail+len(packed)-first]
    require(image[:512]==boot and installed==packed and unpack_kernel(packed,c)==raw,'Build boot/kernel mismatch')
    info=json.loads((build/'build_info.json').read_text())
    require(info.get('dirty') is False,'Frozen production build must be clean')
    return info

def prepare(build,output,window_manifest,workspace_report):
    build=build.resolve();output=output.resolve()
    require(not output.exists(),'Refusing to replace existing evidence')
    build_info=verify_build(build)
    output.mkdir(parents=True)
    manifest=dict(schema=1,status='PREPARING; NO GUEST RUN',preparation_only=True,
        runtime_build_info=build_info,collector_revision=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
        production_guest_qualified=False,collector_source=record(__file__),
        collector_source_dirty=bool(subprocess.check_output(['git','status','--porcelain'],cwd=ROOT)),
        profiles={},apps={},archive_manifests={},phases=[])
    def save(): (output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    save()
    try:
        manifest['build']={}
        for name in ['baseos.img','boot.bin','kernel.bin','kernel.packed','kernel.elf','build_info.json']:
            shutil.copyfile(build/name,output/name);manifest['build'][name]=record(output/name)
        appdata={}
        def copy_app(name,path,entry,origin):
            original=verified_archive(path,entry);shutil.copyfile(path,output/name)
            manifest['apps'][name]=dict(**record(output/name),origin=origin,original=original)
            appdata[name]=(output/name).read_bytes()
        old_manifest=ROOT/'tests/fixtures/bex1-hour05/manifest.json'
        old=json.loads(old_manifest.read_text());manifest['archive_manifests']['bex1']=record(old_manifest)
        for name in ['counter.bex','notebook.bex','hello-c.bex']:
            copy_app(name,old_manifest.parent/name,old['files'][name],'unchanged archived BEX1')
        window=json.loads(window_manifest.read_text());manifest['archive_manifests']['window']=record(window_manifest)
        for name in ['pointer.bex','pointer-window.bex','window-document.bex']:
            entry=window['apps'][name];copy_app(name,Path(entry['path']),entry,'unchanged archived hosted Pointer/owned GUI')
        workspace=json.loads(workspace_report.read_text());manifest['archive_manifests']['workspace']=record(workspace_report)
        name='workspace-array.bex';entry=workspace['prepared_files'][name]
        copy_app(name,workspace_report.parent/name,entry,'unchanged archived hosted BEX2')
        for name,source in [('stage1.bex','staged-document.bex'),('stage2.bex','staged-document2.bex'),('stagewin.bex','staged-document-window.bex'),('reopen.bex','staged-reopen.bex')]:
            entry=record(build/source);copy_app(name,build/source,entry,'new transaction fixture from current clean build')
        old_input=workspace_report.parent/'stats-sample.txt'
        verified_archive(old_input,workspace['prepared_files']['stats-sample.txt'])
        expected=document_bytes();(output/'expected-seed1.txt').write_bytes(expected)
        manifest['expected_content']=record(output/'expected-seed1.txt')
        for profile,ram in [('default',64),('large',128)]:
            folder=output/profile;folder.mkdir();image=folder/'data.img'
            require(initialize(image,profile=profile),'Fixture image already exists')
            nodes={0:dict(parent=-1,name='',directory=1,app=0,data=b'',modified=0),
                   1:dict(parent=0,name='Programs',directory=1,app=0,data=b'',modified=0),
                   2:dict(parent=0,name='Documents',directory=1,app=0,data=b'',modified=0)}
            for name,data in appdata.items():
                nodes[len(nodes)]=dict(parent=1,name=name,directory=0,app=0,data=data,modified=1)
            documents={'staged.txt':document_bytes(0,20000),'unchanged.txt':b'Untouched ordinary peer document.\n',
                       'stats-sample.txt':old_input.read_bytes()}
            documents.update({f'counter-{i}.txt':f'{100+i}\n'.encode() for i in range(1,9)})
            for name,data in documents.items():
                nodes[len(nodes)]=dict(parent=2,name=name,directory=0,app=0,data=data,modified=1)
            layout=data_layout(profile);header,payload=encode_snapshot(nodes,layout,1)
            with image.open('r+b') as f:
                f.seek(layout.lbas[0]*512);f.write(header.ljust(512,b'\0'));f.write(payload)
            _,_,decoded=load(image.read_bytes())
            for name,data in appdata.items():require(decoded[resolve(decoded,'/Programs/'+name)]['data']==data,'Prepared app mismatch')
            for name,data in documents.items():require(decoded[resolve(decoded,'/Documents/'+name)]['data']==data,'Prepared document mismatch')
            manifest['profiles'][profile]=dict(ram_mib=ram,status='NOT RUN',initial_volume=record(image),
                initial_documents={name:dict(bytes=len(data),sha256=digest(data)) for name,data in documents.items()})
        manifest['phases']=[dict(name=name,status='NOT RUN') for name in [
            'old-binary launch/input/save/exit compatibility',
            'BEX1 and BEX2 larger replace/create with exact coherent readback',
            'two owners competing revision and explicit Save As recovery',
            'interrupted upload and exact public free-page recovery',
            'aggregate staging capacity and normal retry',
            'snapshot BUSY with visible peer progress and retry',
            'owned content durability receipts and complete stopped-volume bytes',
            'cold reboot of the same unchanged disks and coherent bytes',
            'external accept-to-visible-result and peer/input stalls at near-full first/middle/last placements']]
        manifest['timing']='External monotonic timestamps from input injection to first published RAM result, plus peer/input intervals; includes input/draw overhead, not isolated syscall time. PIT samples cannot establish elapsed copy duration.'
        manifest['limits']=['Preparation does not qualify a guest collector or runtime.',
            'Near-full latency images are not generated by this functional preparation.',
            'No QEMU, debugger, memory access, corruption, fuzz, security or historical fault lane.']
        manifest['status']='PREPARED ORDINARY FILE TRANSACTIONS; NO GUEST RUN';save()
    except Exception as error:
        manifest['status']='PREPARATION FAILED';manifest['error']=repr(error);save();raise
    return manifest

def verify_stopped(path,document,seed,length):
    with locked_image(path) as f:data=f.read()
    slot,generation,nodes=load(data);actual=nodes[resolve(nodes,document)]['data'];expected=document_bytes(seed,length)
    require(actual==expected,'Stopped volume does not contain every expected byte')
    return dict(status='EXACT STOPPED CONTENT',volume=dict(path=str(Path(path).resolve()),bytes=len(data),sha256=digest(data)),slot=slot,generation=generation,
                document=document,bytes=len(actual),sha256=digest(actual),seed=seed)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build',type=Path);p.add_argument('--output',type=Path)
    p.add_argument('--window-manifest',type=Path);p.add_argument('--workspace-report',type=Path)
    p.add_argument('--verify-stopped',type=Path);p.add_argument('--document',default='/Documents/staged.txt')
    p.add_argument('--seed',type=int,default=1);p.add_argument('--bytes',type=int,default=262144)
    a=p.parse_args()
    if a.verify_stopped:
        require(not any([a.build,a.output,a.window_manifest,a.workspace_report]),'Verification does not prepare or alter media')
        result=verify_stopped(a.verify_stopped,a.document,a.seed,a.bytes)
    else:
        require(all([a.build,a.output,a.window_manifest,a.workspace_report]),'Preparation requires all four input paths')
        result=prepare(a.build,a.output,a.window_manifest,a.workspace_report)
    print(json.dumps(result,indent=2))
if __name__=='__main__':main()
