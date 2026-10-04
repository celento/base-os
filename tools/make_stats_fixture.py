"""Make an original deterministic text document for the native DocStats example."""
import argparse
from pathlib import Path


def document():
    header = 'BaseOS Document Stats sample\nRead it in Editor or stream it from a C application.\n\n'
    rows = [f'Entry {i:04d}: Quiet harbor, bright windows, and {i % 97:02d} little boats.\n'
            for i in range(900)]
    return (header + ''.join(rows) + 'The last line has no newline.').encode('ascii')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    data = document()
    args.output.write_bytes(data)
    print(f'{args.output}: {len(data)} bytes, {len(data.split())} words, {len(data.splitlines())} lines')
