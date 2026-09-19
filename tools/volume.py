"""Offline file exchange. Stop QEMU first.

python3 tools/volume.py build/baseos.img ls /docs
python3 tools/volume.py build/baseos.img import hello.txt /docs/hello.txt
python3 tools/volume.py build/baseos.img export /docs/hello.txt hello.txt
python3 tools/volume.py build/baseos.img mkdir /Projects
"""
import argparse
import datetime
import fcntl
import hashlib
import os
from pathlib import Path
import struct
import tempfile
import zlib
from layout import constants
C = constants()
MAGIC = 0x46534f42
EPOCH = datetime.datetime(2000, 1, 1, tzinfo=datetime.timezone.utc)


def rolling(data):
    n = len(data)
    for byte in data: n = ((n << 1) + (n >> 31) + byte) & 0xffffffff
    return n


def decode(data, slot):
    start = (C['FS_SECOND_LBA'] if slot else C['FS_DISK_LBA']) * 512
    if len(data) < start + C['FS_DISK_SECTORS'] * 512: return None
    magic, version, count, size, crc, gen, hcrc = struct.unpack_from('<7I', data, start)
    if magic != MAGIC or version not in (1, 2, 3) or not 1 <= count <= 64: return None
    if version == 1 and slot: return None
    if version >= 2 and zlib.crc32(data[start:start+24]) != hcrc: return None
    if size > (C['FS_DISK_SECTORS'] - 1) * 512: return None
    payload = data[start+512:start+512+size]
    if (rolling(payload) if version == 1 else zlib.crc32(payload)) != crc: return None
    nodes = {}; pos = 0; record = 40 if version >= 3 else 36
    for _ in range(count):
        if pos + record > len(payload): return None
        ident, parent, directory, app, _, length, raw = struct.unpack_from('<HhBBHI24s', payload, pos)
        modified = struct.unpack_from('<I', payload, pos+36)[0] if version >= 3 else 0
        pos += record
        if ident >= 64 or ident in nodes or parent < -1 or parent >= 64 or directory > 1 or app > 1 or (directory and app): return None
        if length >= 16384 or pos + length > len(payload) or ((directory or app) and length): return None
        if b'\0' not in raw: return None
        name = raw.split(b'\0',1)[0].decode('ascii', errors='strict')
        if '/' in name or (ident and name in ('', '.', '..')): return None
        nodes[ident] = dict(name=name, parent=parent, directory=directory, app=app,
                            data=payload[pos:pos+length], modified=modified)
        pos += length
    if pos != size or 0 not in nodes or nodes[0]['name'] or nodes[0]['parent'] != -1 or not nodes[0]['directory']: return None
    siblings = set()
    for ident, node in nodes.items():
        if not ident: continue
        key = (node['parent'], node['name'])
        if key in siblings: return None
        siblings.add(key); walk = ident; seen = set()
        while walk:
            if walk in seen: return None
            seen.add(walk); walk = nodes[walk]['parent']
            if walk not in nodes or not nodes[walk]['directory']: return None
    return (gen if version >= 2 else 0), nodes


def load(data):
    candidates = [(i, decode(data, i)) for i in range(2)]
    valid = [(i, result) for i, result in candidates if result is not None]
    if not valid: raise ValueError('No valid filesystem snapshot; boot BaseOS once to initialize a blank disk.')
    slot, (gen, nodes) = valid[0]
    for i, (other, n) in valid[1:]:
        if 0 < ((other-gen) & 0xffffffff) < 0x80000000: slot, gen, nodes = i, other, n
    return slot, gen, nodes


def resolve(nodes, path):
    ident = 0
    for name in path.split('/'):
        if name in ('', '.'): continue
        if name == '..': ident = max(0, nodes[ident]['parent']); continue
        if not nodes[ident]['directory']: raise ValueError('Not a directory')
        ident = next((i for i,n in nodes.items() if n['parent'] == ident and n['name'] == name), -1)
        if ident < 0: raise ValueError(f'Path not found: {path}')
    return ident


def commit(image, data, slot, generation, nodes):
    if len(data) != C['DISK_SECTORS']*512: raise ValueError('Run make to upgrade the image before importing.')
    payload = bytearray()
    for ident,n in sorted(nodes.items()):
        payload += struct.pack('<HhBBHI24sI', ident, n['parent'], n['directory'], n['app'], 0,
                               len(n['data']), n['name'].encode('ascii'), n['modified']) + n['data']
    if len(payload) > (C['FS_DISK_SECTORS']-1)*512: raise ValueError('Volume full')
    header = struct.pack('<6I', MAGIC, 3, len(nodes), len(payload), zlib.crc32(payload), (generation+1)&0xffffffff)
    header += struct.pack('<I', zlib.crc32(header))
    start = (C['FS_SECOND_LBA'] if slot == 0 else C['FS_DISK_LBA'])*512
    updated = bytearray(data)
    updated[start:start+512] = header.ljust(512,b'\0')
    updated[start+512:start+512+len(payload)] = payload
    assert decode(updated, 1-slot) is not None
    backup = image.with_name(image.name+'.'+hashlib.sha256(data).hexdigest()[:16]+'.bak')
    if not backup.exists():
        with backup.open('xb') as f: f.write(data); f.flush(); os.fsync(f.fileno())
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=image.parent,delete=False) as f:
            temporary = Path(f.name)
            fcntl.lockf(f,fcntl.LOCK_EX|fcntl.LOCK_NB)
            f.write(updated); f.flush(); os.fsync(f.fileno())
            os.replace(temporary,image); temporary=None
            fd=os.open(image.parent,os.O_RDONLY)
            try: os.fsync(fd)
            finally: os.close(fd)
    finally:
        if temporary: temporary.unlink(missing_ok=True)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image',type=Path)
    sub=parser.add_subparsers(dest='command',required=True)
    p=sub.add_parser('ls'); p.add_argument('path',default='/',nargs='?')
    p=sub.add_parser('import'); p.add_argument('host',type=Path); p.add_argument('path'); p.add_argument('--replace',action='store_true')
    p=sub.add_parser('export'); p.add_argument('path'); p.add_argument('host',type=Path); p.add_argument('--replace',action='store_true')
    p=sub.add_parser('mkdir'); p.add_argument('path')
    a=parser.parse_args()
    try:
        with a.image.open('r+b') as f:
            fcntl.lockf(f,fcntl.LOCK_EX|fcntl.LOCK_NB)
            data=f.read(); slot,gen,nodes=load(data)
            if a.command=='ls':
                ident=resolve(nodes,a.path)
                for i,n in nodes.items():
                    if n['parent']==ident:
                        stamp = str(EPOCH+datetime.timedelta(seconds=n['modified'])) if n['modified'] else 'unknown'
                        print(f"{'dir' if n['directory'] else 'file':4} {len(n['data']):6} {stamp} {n['name']}")
            elif a.command=='export':
                n=nodes[resolve(nodes,a.path)]
                if n['directory'] or n['app']: raise ValueError('Not a data file')
                with a.host.open('wb' if a.replace else 'xb') as out: out.write(n['data'])
            else:
                parent_path,_,name=a.path.rstrip('/').rpartition('/')
                parent=resolve(nodes,parent_path)
                if not nodes[parent]['directory'] or name in ('','.','..') or not 0<len(name.encode('ascii'))<24: raise ValueError('Invalid destination')
                ident=next((i for i,n in nodes.items() if n['parent']==parent and n['name']==name),None)
                if ident is not None and (a.command=='mkdir' or not a.replace or nodes[ident]['directory'] or nodes[ident]['app']): raise ValueError('Destination exists; use --replace for a data file')
                if ident is None: ident=next((i for i in range(1,64) if i not in nodes),None)
                if ident is None: raise ValueError('No free file slots')
                content=a.host.read_bytes() if a.command=='import' else b''
                if len(content)>=16384: raise ValueError('File exceeds 16,383 bytes')
                nodes[ident]=dict(name=name,parent=parent,directory=int(a.command=='mkdir'),app=0,data=content,modified=int((datetime.datetime.now(datetime.timezone.utc)-EPOCH).total_seconds()))
                commit(a.image,data,slot,gen,nodes)
    except (OSError,ValueError,UnicodeError) as e: parser.exit(1,f'volume: {e}\n')

if __name__=='__main__': main()
