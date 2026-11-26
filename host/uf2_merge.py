#!/usr/bin/env python3
"""Merge UF2 files and binary files into one UF2, e.g. the firmware and the
font, so one drag-and-drop onto the RP2350's BOOTSEL drive flashes both.

Usage:
  host/uf2_merge.py OUT.uf2 IN...

Each IN is a UF2 file, or FILE@ADDRESS: a binary file to write at that
address (0x10000000 is the start of flash), e.g. the font at 2 MB:

  host/uf2_merge.py fsynth+font.uf2 build/pico_fsynth.uf2 build-host/font.fsf@0x10200000
  cp fsynth+font.uf2 /media/$USER/RP2350/ && sync

or the font alone (as picotool uf2 convert ... -t bin -o 0x10200000 does):

  host/uf2_merge.py font.uf2 build-host/font.fsf@0x10200000

Binary files get the family of the UF2 inputs, or rp2350-arm-s (0xe48bff59)
if there are none.

Copying two UF2 files one after the other does not work: the boot ROM reboots
as soon as it has every block of the first one. A merged file has one block
count. Each block says "block i of n", so the data blocks are renumbered
0..n-1 across the inputs.

Blocks of the "absolute" family (0xe48bff57) are copied first, unchanged.
picotool and the SDK put one at the start of RP2350 UF2s as a workaround for
the RP2350-E10 boot ROM erratum.
"""

import struct
import sys

UF2_MAGIC = (0x0A324655, 0x9E5D5157)
UF2_END = 0x0AB16F30
ABSOLUTE = 0xE48BFF57
RP2350_ARM_S = 0xE48BFF59
FLAG_FAMILY = 0x2000


def blocks(path):
    data = open(path, 'rb').read()
    if len(data) % 512:
        sys.exit('%s: not a UF2 file (size not a multiple of 512)' % path)
    for i in range(0, len(data), 512):
        b = data[i:i + 512]
        m0, m1, flags, addr, size, _, _, family = struct.unpack('<8I', b[:32])
        if (m0, m1) != UF2_MAGIC or struct.unpack('<I', b[508:])[0] != UF2_END:
            sys.exit('%s: block %d: bad UF2 magic' % (path, i // 512))
        yield b, addr, size, family


def bin_blocks(path, base, family):
    """256-byte data blocks of a binary file, the last one padded with zeros."""
    data = open(path, 'rb').read()
    for off in range(0, len(data), 256):
        payload = data[off:off + 256].ljust(256, b'\0')
        b = (struct.pack('<8I', UF2_MAGIC[0], UF2_MAGIC[1], FLAG_FAMILY, base + off, 256, 0, 0, family)
             + payload + bytes(476 - 256) + struct.pack('<I', UF2_END))
        yield b, base + off, 256, family


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    out, inputs = sys.argv[1], sys.argv[2:]
    head, data, bins = [], [], []
    for p in inputs:
        if '@' in p:
            path, addr = p.rsplit('@', 1)
            bins.append((path, int(addr, 0)))
            continue
        for b, addr, size, family in blocks(p):
            if family == ABSOLUTE:
                if not any(h == b for h in head):
                    head.append(b)
            else:
                data.append((b, addr, size, family))
    uf2_families = {f for _, _, _, f in data}
    family = uf2_families.pop() if len(uf2_families) == 1 else RP2350_ARM_S
    for path, addr in bins:
        data.extend(bin_blocks(path, addr, family))
    families = {f for _, _, _, f in data}
    if len(families) != 1:
        sys.exit('expected one family across the inputs, found %s'
                 % ', '.join('0x%08x' % f for f in sorted(families)))
    spans = sorted((addr, addr + size) for _, addr, size, _ in data)
    for (s0, e0), (s1, _) in zip(spans, spans[1:]):
        if s1 < e0:
            sys.exit('inputs overlap at 0x%08x' % s1)
    n = len(data)
    with open(out, 'wb') as f:
        for b in head:
            f.write(b)
        for i, (b, _, _, _) in enumerate(data):
            f.write(b[:20] + struct.pack('<II', i, n) + b[28:])
    print('%s: %d + %d blocks, %.1f MB, flash 0x%08x..0x%08x'
          % (out, len(head), n, (len(head) + n) * 512 / 1e6, spans[0][0], spans[-1][1]))


if __name__ == '__main__':
    main()
