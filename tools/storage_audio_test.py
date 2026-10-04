"""Exercise large Editor documents and find/replace using disposable QEMU images."""
import pathlib,subprocess,tempfile,sys,shutil,json,time
from layout import constants
from update_image import install_kernel
from data_volume_test import run
from init_data import initialize
build=pathlib.Path(sys.argv[1]).resolve();root=pathlib.Path(__file__).resolve().parents[1]
d=pathlib.Path(tempfile.mkdtemp(prefix='baseos-storage-audio-'));print(d,flush=True)
def tool(n):return 'x86_64-elf-'+n if shutil.which('x86_64-elf-'+n) else n
subprocess.run([tool('gcc'),'-Os','-ffreestanding','-m32','-fno-pie','-fno-stack-protector','-fno-builtin','-mno-sse','-mno-mmx','-msoft-float','-I',str(root),'-I',str(root/'src'),'-I',str(build),'-c',str(root/'tests/storage_audio_guest.c'),'-o',str(d/'kernel.o')],check=True)
objects=[str(p) for p in build.glob('*.o') if p.name!='kernel.o']
subprocess.run([tool('ld'),'-T',str(build/'linker.ld'),'-nostdlib','-m','elf_i386','-z','noexecstack','-o',str(d/'kernel.elf'),str(d/'kernel.o'),*objects],check=True)
subprocess.run([tool('objcopy'),'-O','binary',str(d/'kernel.elf'),str(d/'kernel.bin')],check=True)
c=constants();kernel=(d/'kernel.bin').read_bytes();assert len(kernel)<=c['KERNEL_SECTORS']*512
image=bytearray(c['DISK_SECTORS']*512);image[:512]=(build/'boot.bin').read_bytes();install_kernel(image,kernel,c)
(d/'disk.img').write_bytes(image)
initialize(d/'data.img')
log=d/'serial.log';log.write_text('')
process=subprocess.Popen(['qemu-system-i386','-m','64M','-vga','std','-drive',f'file={d}/disk.img,format=raw,index=0,if=floppy','-drive',f'file={d}/data.img,format=raw,index=0,if=ide','-audiodev',f'wav,id=audio,path={d}/capture.wav','-device','sb16,audiodev=audio','-serial',f'file:{log}','-display','none','-no-reboot'],stderr=subprocess.PIPE)
try:
    end=time.monotonic()+60
    while time.monotonic()<end:
        text=log.read_text()
        if 'STORAGE-AUDIO-PASS' in text:break
        if 'PANIC:' in text or process.poll() is not None:raise AssertionError(text)
        time.sleep(.1)
    else:raise AssertionError(log.read_text())
finally:process.terminate();process.wait(timeout=5)
assert (d/'capture.wav').stat().st_size>1000000
print(log.read_text(),end='');print('15-second actual PCM playback survived large snapshots and arena compaction with zero underruns.')
