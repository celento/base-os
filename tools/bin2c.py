"""Generate a C byte array for the bundled, reproducibly assembled native demo."""
import pathlib,sys
blob=pathlib.Path(sys.argv[1]).read_bytes()
name = sys.argv[2] if len(sys.argv) > 2 else 'native_example'
if not name.isidentifier(): raise ValueError('invalid C array name')
print(f'static const unsigned char {name}[] = {{')
for i in range(0,len(blob),16):print('    '+','.join(f'0x{x:02x}' for x in blob[i:i+16])+',')
print('};')
