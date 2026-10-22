"""PS/2 packet counts for exact pointer motion at the default pointer speed.

Mirrors input_pointer_scale in src/kernel/input_ingress.c, reading its
constants from the header, so tests can move the pointer by an exact
number of pixels at any default speed level."""
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parents[1]


def _header():
    text = (ROOT / 'src/kernel/input_ingress.h').read_text()
    level = int(re.search(r'#define\s+INPUT_SPEED_DEFAULT\s+(\d+)', text).group(1))
    precision = int(re.search(r'#define\s+INPUT_PRECISION\s+(\d+)', text).group(1))
    table = re.search(r'#define\s+INPUT_SPEED_EIGHTHS\s+\{([^}]*)\}', text).group(1)
    return [int(v) for v in table.split(',')][level], precision


EIGHTHS, PRECISION = _header()


def scale(counts):
    """Pixels the kernel moves for one packet of `counts` on one axis."""
    n = abs(counts)
    if n > PRECISION:
        n = PRECISION + (n - PRECISION) * EIGHTHS // 8
    return -n if counts < 0 else n


def step(pixels, limit=80):
    """The largest packet, at most `limit` counts, that moves no further than
    `pixels`. Zero for zero; never overshoots."""
    if not pixels:
        return 0
    sign, want = (-1 if pixels < 0 else 1), abs(pixels)
    best = 1
    for counts in range(1, limit + 1):
        if scale(counts) <= want:
            best = counts
        else:
            break
    return sign * best


def steps(pixels, limit=80):
    """Packets that together move exactly `pixels` on one axis."""
    out = []
    while pixels:
        counts = step(pixels, limit)
        out.append(counts)
        pixels -= scale(counts)
    return out


def counts_for(pixels):
    """The single packet that moves exactly `pixels`. Raises if none does."""
    counts = step(pixels, 127)
    if scale(counts) != pixels:
        raise ValueError(f'no single PS/2 packet moves exactly {pixels} px')
    return counts


def rel_events(dx, dy, limit=80):
    """QMP input-send-event batches that move exactly (dx, dy) pixels."""
    xs, ys = steps(dx, limit), steps(dy, limit)
    batches = []
    for i in range(max(len(xs), len(ys))):
        batch = [{'type': 'rel', 'data': {'axis': axis, 'value': values[i]}}
                 for axis, values in (('x', xs), ('y', ys)) if i < len(values)]
        batches.append(batch)
    return batches


def release_split(dx, dy):
    """Exact motion ending in one packet that carries most of it: QMP batches
    to send first, and the final packet's events (to pair with a button)."""
    fx, fy = step(dx, 127), step(dy, 127)
    lead = rel_events(dx - scale(fx), dy - scale(fy))
    final = [{'type': 'rel', 'data': {'axis': axis, 'value': value}}
             for axis, value in (('x', fx), ('y', fy)) if value]
    return lead, final
