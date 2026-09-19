"""The integer-only shared layout; reject ambiguous configuration."""
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parents[1]

def constants():
    text = (ROOT / 'src/layout.h').read_text()
    return {k: int(v, 0) for k, v in re.findall(
        r'^#define\s+(\w+)\s+(0x[0-9A-Fa-f]+|[0-9]+)\s*$', text, re.M)}

if __name__ == '__main__':
    for name, value in constants().items():
        print(f'%define {name} {value}')
