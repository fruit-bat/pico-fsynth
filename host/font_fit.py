#!/usr/bin/env python3
"""Fit as many presets of a SoundFont as possible into a flash budget.

Usage:
  host/font_fit.py [options] SOURCE.sf2 OUT.fsf

Converts SOURCE with sf2conv, keeping the candidate presets (by default the
GM set: bank 0 and drum kit 128:0). If that is too big for --size, it picks
presets greedily, cheapest first: each step adds the preset that needs the
fewest new sample frames, so presets that share samples with ones already
kept are nearly free. 0:0 (the fallback for missing programs), 128:0 (the
fallback drum kit) and any --force presets always go first. The preset count
is then binary searched with real conversions to get the largest font that
fits.

Writes OUT.fsf, and OUT.keep with the chosen list (for sf2conv --keep), and
prints the presets that were left out. A missing program falls back to bank
0, then to 0:0, so music that uses a missing one plays it on the piano.

Options:
  --size BYTES      flash space for the font (default 0xDFF000: the RP2350's
                    16 MB, less 2 MB of firmware and the 4 KB settings sector)
  --candidates LIST presets to choose from, sf2conv --keep syntax
                    (default 0,128:0)
  --force LIST      bank:program presets to keep whatever their cost
  --compress MODE   sf2conv --compress mode: fast (default), small or none
  --tools DIR       where sf2conv and fsfinfo are (default build-host)
  any other option starting with -- is passed to sf2conv (e.g. --vel-filter=fluid)

Example (the 16 MB RP2350-Plus font, 105 of 129 presets):
  host/font_fit.py --force 0:24,0:44,0:48,0:49,0:73 \\
      GeneralUser-GS.sf2 build-host/GeneralUser-GM-pico.fsf
"""

import os
import re
import subprocess
import sys
import tempfile


def die(msg):
    sys.exit('font_fit: ' + msg)


def parse_args(argv):
    opts = {'size': 0xDFF000, 'candidates': '0,128:0', 'force': '', 'compress': 'fast',
            'tools': 'build-host', 'extra': []}
    pos = []
    i = 0
    while i < len(argv):
        a = argv[i]
        name = a[2:].split('=', 1)[0] if a.startswith('--') else None
        if name in ('size', 'candidates', 'force', 'compress', 'tools'):
            if '=' in a:
                val = a.split('=', 1)[1]
            else:
                i += 1
                if i == len(argv):
                    die('%s needs a value' % a)
                val = argv[i]
            opts[name] = int(val, 0) if name == 'size' else val
        elif a in ('-h', '--help'):
            print(__doc__)
            sys.exit(0)
        elif a.startswith('--'):
            opts['extra'].append(a)
        else:
            pos.append(a)
        i += 1
    if len(pos) != 2:
        die('usage: font_fit.py [options] SOURCE.sf2 OUT.fsf (--help for more)')
    opts['src'], opts['out'] = pos
    return opts


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        die('%s failed:\n%s%s' % (' '.join(cmd), r.stdout, r.stderr))
    return r.stdout


def convert(o, keep, out):
    cmd = [os.path.join(o['tools'], 'sf2conv')]
    if o['compress'] != 'none':
        cmd.append('--compress=' + o['compress'])
    run(cmd + o['extra'] + ['--keep=' + keep, o['src'], out])
    return os.path.getsize(out)


def presets(o, fsf):
    """{(bank, program): (name, [(first frame, end frame), ...])}"""
    fsfinfo = os.path.join(o['tools'], 'fsfinfo')
    res = {}
    for line in run([fsfinfo, fsf]).splitlines()[1:]:
        m = re.match(r'\s*(\d+):\s*(\d+)\s+(.*?)\s+\d+ regions', line)
        if not m:
            continue
        k = (int(m[1]), int(m[2]))
        regions = run([fsfinfo, fsf, str(k[0]), str(k[1])])
        res[k] = (m[3], [(int(a), int(b)) for a, b in re.findall(r'frames (\d+)\.\.(\d+)', regions)])
    return res


def frames(spans):
    """Sample frames covered by a list of (start, end) spans, overlaps counted once."""
    total, end = 0, -1
    for s, e in sorted(spans):
        if e > end:
            total += e - max(s, end)
            end = e
    return total


def keep_list(keys):
    return ','.join('%d:%d' % k for k in sorted(keys))


def main():
    o = parse_args(sys.argv[1:])
    with tempfile.TemporaryDirectory() as tmp:
        trial = os.path.join(tmp, 'trial.fsf')
        full = convert(o, o['candidates'], trial)
        print('all %s presets: %d bytes (%.2f MB), budget %d bytes (%.2f MB)'
              % (o['candidates'], full, full / 1e6, o['size'], o['size'] / 1e6))
        info = presets(o, trial)
        if full <= o['size']:
            order = sorted(info)
        else:
            # Greedy order: the forced presets, then cheapest next.
            first = [(0, 0), (128, 0)]
            for f in filter(None, o['force'].split(',')):
                k = tuple(int(x) for x in f.split(':'))
                if k not in info:
                    die('--force %s: not among the candidates' % f)
                if k not in first:
                    first.append(k)
            order = [k for k in first if k in info]
            spans = [s for k in order for s in info[k][1]]
            rest = set(info) - set(order)
            while rest:
                base = frames(spans)
                k = min(rest, key=lambda k: (frames(spans + info[k][1]) - base, k))
                order.append(k)
                spans += info[k][1]
                rest.remove(k)
            # Largest prefix of the order that fits, by binary search.
            lo, hi = len([k for k in first if k in info]), len(order)
            if convert(o, keep_list(order[:lo]), trial) > o['size']:
                die('the forced presets alone do not fit')
            while hi - lo > 1:
                mid = (lo + hi) // 2
                size = convert(o, keep_list(order[:mid]), trial)
                print('  %3d presets: %d bytes, %s' % (mid, size, 'fits' if size <= o['size'] else 'too big'))
                if size <= o['size']:
                    lo = mid
                else:
                    hi = mid
            order = order[:lo]
        keep = keep_list(order)
        size = convert(o, keep, o['out'])
    base = os.path.splitext(o['out'])[0]
    with open(base + '.keep', 'w') as f:
        f.write(keep + '\n')
    print('%s: %d of %d presets, %d bytes (%.2f MB), %d bytes spare'
          % (o['out'], len(order), len(info), size, size / 1e6, o['size'] - size))
    print('%s.keep: the list, for sf2conv --keep=$(cat %s.keep)' % (base, base))
    missing = sorted(set(info) - set(order))
    if missing:
        print('left out (play on the bank 0 preset, or 0:0):')
        for k in missing:
            print('  %3d:%-3d %s' % (k[0], k[1], info[k][0]))


if __name__ == '__main__':
    main()
