#!/usr/bin/env python3
"""Merge UF2 files into one, e.g. the firmware and the font, so one
drag-and-drop onto the RP2350's BOOTSEL drive flashes both.

Usage:
  host/uf2_merge.py OUT.uf2 IN.uf2...

  picotool uf2 convert build-host/font.fsf -t bin font.uf2 -o 0x10200000 --family rp2350-arm-s
  host/uf2_merge.py fsynth+font.uf2 build/pico_fsynth.uf2 font.uf2
  cp fsynth+font.uf2 /media/$USER/RP2350/ && sync

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


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    out, inputs = sys.argv[1], sys.argv[2:]
    head, data = [], []
    for p in inputs:
        for b, addr, size, family in blocks(p):
            if family == ABSOLUTE:
                if not any(h == b for h in head):
                    head.append(b)
            else:
                data.append((b, addr, size, family))
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
