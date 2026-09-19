import pathlib,sys,tempfile,unittest,subprocess,struct,zlib
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]/'tools'))
import volume
ROOT=pathlib.Path(__file__).resolve().parents[1]
class VolumeTests(unittest.TestCase):
    def fixture(self,version):
        image=bytearray(volume.C['DISK_SECTORS']*512)
        payload=struct.pack('<HhBBHI24s',0,-1,1,0,0,0,b'')+(struct.pack('<I',0) if version==3 else b'')
        payload+=struct.pack('<HhBBHI24s',1,0,0,0,0,3,b'old.txt')+(struct.pack('<I',1234) if version==3 else b'')+b'old'
        header=struct.pack('<6I',volume.MAGIC,version,2,len(payload),volume.rolling(payload) if version==1 else zlib.crc32(payload),1)
        header+=struct.pack('<I',0 if version==1 else zlib.crc32(header))
        start=volume.C['FS_DISK_LBA']*512;image[start:start+28]=header;image[start+512:start+512+len(payload)]=payload
        return image
    def cli(self,image,*args,ok=True):
        result=subprocess.run([sys.executable,str(ROOT/'tools/volume.py'),str(image),*map(str,args)],capture_output=True,text=True)
        self.assertEqual(result.returncode==0,ok,result.stderr)
        return result.stdout
    def test_versions_import_export_backups(self):
        for version in (1,2,3):
            with self.subTest(version=version),tempfile.TemporaryDirectory() as temp:
                d=pathlib.Path(temp);image=d/'disk';original=self.fixture(version);image.write_bytes(original)
                self.assertEqual(volume.load(original)[2][1]['data'],b'old')
                self.cli(image,'mkdir','/Test')
                host=d/'binary';content=bytes(range(256))*63+b'\0last';host.write_bytes(content)
                self.cli(image,'import',host,'/Test/binary')
                output=d/'export';self.cli(image,'export','/Test/binary',output);self.assertEqual(output.read_bytes(),content)
                self.cli(image,'export','/Test/binary',output,ok=False)
                before=image.read_bytes();self.cli(image,'import',host,'/old.txt',ok=False);self.assertEqual(image.read_bytes(),before)
                self.cli(image,'import',host,'/old.txt','--replace');self.assertEqual(volume.load(image.read_bytes())[2][1]['data'],content)
                self.assertTrue(any(p.read_bytes()==original for p in d.glob('*.bak')))
                host.write_bytes(bytes(16384));before=image.read_bytes();self.cli(image,'import',host,'/too-big',ok=False);self.assertEqual(image.read_bytes(),before)
    def test_corrupt_latest_falls_back(self):
        with tempfile.TemporaryDirectory() as temp:
            d=pathlib.Path(temp);image=d/'disk';image.write_bytes(self.fixture(3));self.cli(image,'mkdir','/new')
            data=bytearray(image.read_bytes());data[volume.C['FS_SECOND_LBA']*512+520]^=1
            self.assertEqual(volume.load(data)[0],0)
    def test_image_lock(self):
        with tempfile.TemporaryDirectory() as temp:
            image=pathlib.Path(temp)/'disk';image.write_bytes(self.fixture(3))
            child=subprocess.Popen([sys.executable,'-c','import fcntl,sys; f=open(sys.argv[1],"r+b"); fcntl.lockf(f,fcntl.LOCK_EX); print("ready",flush=True); sys.stdin.read()',str(image)],stdin=subprocess.PIPE,stdout=subprocess.PIPE)
            try:
                self.assertEqual(child.stdout.readline().strip(),b'ready');before=image.read_bytes();self.cli(image,'mkdir','/blocked',ok=False);self.assertEqual(image.read_bytes(),before)
            finally:child.communicate(timeout=5)
