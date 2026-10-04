"""Ordinary additive ABI capability/fallback checks on real backend contexts."""
import json
from pathlib import Path

from platform_evidence import PlatformSession, provenance, sha
from volume import C, FLOPPY_LAYOUT, encode_snapshot
from update_image import install_packed_kernel


def floppy_fixture(build, directory, app):
    directory=Path(directory);directory.mkdir(parents=True,exist_ok=True)
    nodes={0:dict(parent=-1,name='',directory=1,app=0,data=b'',modified=0),
           1:dict(parent=0,name='Programs',directory=1,app=0,data=b'',modified=0),
           2:dict(parent=0,name='Documents',directory=1,app=0,data=b'',modified=0),
           3:dict(parent=1,name='platform.bex',directory=0,app=0,data=Path(app).read_bytes(),modified=1),
           4:dict(parent=2,name='platform.bin',directory=0,app=0,data=b'A'*8192,modified=1)}
    raw=bytearray(C['DISK_SECTORS']*512);raw[:512]=(Path(build)/'boot.bin').read_bytes()
    install_packed_kernel(raw,(Path(build)/'kernel.packed').read_bytes(),C)
    header,payload=encode_snapshot(nodes,FLOPPY_LAYOUT,1);offset=FLOPPY_LAYOUT.lbas[0]*512
    raw[offset:offset+512]=header.ljust(512,b'\0');raw[offset+512:offset+512+len(payload)]=payload
    image=directory/'floppy.img';image.write_bytes(raw);return image


def run_contexts(build,work,app,old_build=None):
    work=Path(work);work.mkdir(parents=True,exist_ok=True)
    image=floppy_fixture(build,work/'floppy',app)
    result={}
    with PlatformSession(build,'platform-floppy-context',image=image) as session:
        result['floppy_directory']=str(session.directory);session.boot();session.launch('terminal')
        session.text('start /Programs/platform.bex');session.key('ret');session.key('alt-ret')
        session.until(lambda o:o['page']==0,'platform app on floppy')
        caps=session.page(1);limits=session.page(2)
        assert (caps['features']&15)==9 and caps['context']==2 and caps['file_bytes']==16383
        assert caps['replace_bytes']==16383 and caps['user_bytes']==65536 and caps['image_bytes']==49152
        assert limits['operations_per_process']==limits['operations_total']==limits['wait_ms']==0
        session.page(0);session.key('s')
        unsupported=session.until(lambda o:o['sync_result']==-1000,'floppy async reports unsupported')[0]
        session.key('q');session.text('exec /Programs/platform.bex');session.key('ret')
        legacy=session.until(lambda o:o['page']==1 and o['context']==1,'legacy exec truthful capability query')[0]
        assert (legacy['features']&15)==9 and legacy['process']!=caps['process'] and legacy['slot']==0
        result.update(floppy_task=caps,floppy_limits=limits,floppy_async=unsupported,floppy_exec=legacy)
        assert 'PANIC:' not in session.log.read_text()
    result['floppy_sha256']=sha(image.read_bytes())
    if old_build:
        result['old_build']=provenance(old_build)
        old_image=floppy_fixture(old_build,work/'old-kernel',app)
        with PlatformSession(old_build,'platform-old-query',image=old_image) as old:
            result['old_directory']=str(old.directory);old.boot();old.launch('terminal')
            old.text('start /Programs/platform.bex');old.key('ret');old.key('alt-ret')
            fallback=old.until(lambda o:o['page']==0 and o['file_result']==-1,'actual older kernel query fallback')[0]
            assert fallback['process']==0 and fallback['operation']==0 and fallback['errors']==0
            result['old_query_fallback']=fallback
            assert 'PANIC:' not in old.log.read_text()
        result['old_floppy_sha256']=sha(old_image.read_bytes())
    result['passed']=True
    (work/'contexts.json').write_text(json.dumps(result,indent=2)+'\n')
    return result
