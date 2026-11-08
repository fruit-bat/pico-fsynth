#!/usr/bin/env python3
"""Generate small MIDI files that exercise specific synth features.

Usage: make_test_midi.py OUTPUT_DIR
"""
import os
import struct
import sys

PPQ = 480


def vlq(n):
    out = [n & 0x7F]
    n >>= 7
    while n:
        out.append((n & 0x7F) | 0x80)
        n >>= 7
    return bytes(reversed(out))


class Track:
    def __init__(self):
        self.events = []  # (tick, order, bytes)

    def add(self, tick, data):
        self.events.append((tick, len(self.events), bytes(data)))

    def note(self, tick, ch, key, vel, length):
        self.add(tick, [0x90 | ch, key, vel])
        self.add(tick + length, [0x80 | ch, key, 0])

    def cc(self, tick, ch, num, val):
        self.add(tick, [0xB0 | ch, num, val])

    def program(self, tick, ch, prog):
        self.add(tick, [0xC0 | ch, prog])

    def bend(self, tick, ch, value):
        self.add(tick, [0xE0 | ch, value & 0x7F, (value >> 7) & 0x7F])

    def tempo(self, tick, bpm):
        us = int(60_000_000 / bpm)
        self.add(tick, [0xFF, 0x51, 3, (us >> 16) & 0xFF, (us >> 8) & 0xFF, us & 0xFF])

    def encode(self):
        out = bytearray()
        last = 0
        for tick, _, data in sorted(self.events):
            out += vlq(tick - last) + data
            last = tick
        out += vlq(0) + bytes([0xFF, 0x2F, 0])
        return b"MTrk" + struct.pack(">I", len(out)) + bytes(out)


def write(path, track):
    with open(path, "wb") as f:
        f.write(b"MThd" + struct.pack(">IHHH", 6, 0, 1, PPQ))
        f.write(track.encode())


Q = PPQ  # Quarter note


def scale():
    """Piano: C major scale over two octaves, rising velocity, then a held chord."""
    t = Track()
    t.tempo(0, 120)
    t.program(0, 0, 0)
    steps = [0, 2, 4, 5, 7, 9, 11]
    keys = [48 + o * 12 + s for o in range(2) for s in steps] + [72]
    for i, k in enumerate(keys):
        t.note(i * Q // 2, 0, k, 30 + i * 6, Q // 2)
    base = len(keys) * Q // 2
    for k in (48, 60, 64, 67, 72):
        t.note(base, 0, k, 100, Q * 4)
    return t


def gm_tour():
    """Every melodic GM program: a short note and a chord."""
    t = Track()
    t.tempo(0, 120)
    tick = 0
    for prog in range(128):
        t.program(tick, 0, prog)
        t.note(tick + 10, 0, 60, 100, Q // 2)
        for k in (60, 64, 67):
            t.note(tick + Q // 2, 0, k, 90, Q // 2)
        tick += Q * 3 // 2
    return t


def drums():
    """Drum kit pattern including closed/open hi-hat (exclusive class)."""
    t = Track()
    t.tempo(0, 110)
    ch = 9
    for bar in range(4):
        b = bar * Q * 4
        for beat in range(4):
            if beat in (0, 2):
                t.note(b + beat * Q, ch, 36, 110, Q // 4)        # Kick
            if beat in (1, 3):
                t.note(b + beat * Q, ch, 38, 100, Q // 4)        # Snare
            for e in range(2):
                hat = 46 if (beat == 3 and e == 1) else 42       # Open hat cut by closed
                t.note(b + beat * Q + e * Q // 2, ch, hat, 80 + 20 * (e == 0), Q // 4)
    t.note(4 * 4 * Q, ch, 49, 110, Q)                             # Crash
    t.note(4 * 4 * Q, ch, 36, 110, Q // 4)
    return t


def expression():
    """Controllers: pitch bend, mod wheel, volume, expression, pan, sustain."""
    t = Track()
    t.tempo(0, 120)
    t.program(0, 0, 48)  # Strings
    # Pitch bend sweep +2 semitones and back over a held note
    t.note(0, 0, 60, 100, Q * 4)
    for i in range(65):
        v = 8192 + int(8191 * (i / 32 if i <= 32 else (64 - i) / 32))
        t.bend(i * Q * 4 // 64, 0, v)
    t.bend(Q * 4, 0, 8192)
    # Mod wheel vibrato
    t.note(Q * 5, 0, 67, 100, Q * 4)
    for i in range(33):
        t.cc(Q * 5 + i * Q * 4 // 32, 0, 1, min(127, i * 4))
    t.cc(Q * 9, 0, 1, 0)
    # Volume fade out, pan sweep
    t.program(Q * 10, 0, 0)
    t.cc(Q * 10, 0, 64, 127)  # Sustain on
    for k in (48, 55, 60, 64, 67, 72):
        t.note(Q * 10, 0, k, 100, Q // 4)  # Short notes held by the pedal
    for i in range(65):
        t.cc(Q * 10 + i * Q * 4 // 64, 0, 10, min(127, i * 2))
        t.cc(Q * 10 + i * Q * 4 // 64, 0, 7, max(0, 100 - i))
    t.cc(Q * 15, 0, 64, 0)  # Sustain off
    t.cc(Q * 15, 0, 7, 100)
    t.cc(Q * 15, 0, 10, 64)
    return t


def poly():
    """Dense sustained chords on several channels to stress voice allocation."""
    t = Track()
    t.tempo(0, 100)
    progs = [0, 48, 52, 19, 89, 61]
    for ch, p in enumerate(progs):
        t.program(0, ch, p)
    chords = [(48, 52, 55, 60, 64, 67), (45, 52, 57, 60, 64, 69), (41, 48, 53, 57, 60, 65), (43, 50, 55, 59, 62, 67)]
    for i, chord in enumerate(chords * 2):
        for ch in range(len(progs)):
            for k in chord:
                t.note(i * Q * 2, ch, k + (12 if ch % 2 else 0), 90, Q * 3)
    return t


def solo_pad():
    """A solo flute line (channel 0) over dense pads on channels 1-5.

    Needs far more voices than 64 so the stealing policy has to choose; the
    melody should survive.
    """
    t = Track()
    t.tempo(0, 100)
    t.program(0, 0, 73)  # Flute
    for ch, p in zip(range(1, 6), (48, 49, 89, 91, 52)):
        t.program(0, ch, p)
    chords = [(48, 52, 55, 60, 64, 67, 72), (45, 52, 57, 60, 64, 69, 72), (41, 48, 53, 57, 60, 65, 69), (43, 50, 55, 59, 62, 67, 71)]
    for i, chord in enumerate(chords * 2):
        for ch in range(1, 6):
            for k in chord:
                t.note(i * Q * 2, ch, k, 80, Q * 2)
    melody = [72, 74, 76, 79, 77, 76, 74, 72, 71, 72, 74, 76, 74, 72, 71, 69]
    for i in range(len(chords) * 2 * 4):
        t.note(i * Q // 2, 0, melody[i % len(melody)], 100, Q // 2)
    return t


def fx_reverb():
    """Short piano and snare hits, far apart, reverb send full, no chorus."""
    t = Track()
    t.tempo(0, 120)
    t.program(0, 0, 0)
    t.cc(0, 0, 91, 127)
    t.cc(0, 0, 93, 0)
    t.cc(0, 9, 91, 127)
    t.cc(0, 9, 93, 0)
    for i, k in enumerate((48, 60, 72)):
        t.note(Q * 2 + i * Q * 8, 0, k, 110, Q // 8)
    t.note(Q * 26, 9, 38, 120, Q // 8)
    t.note(Q * 34, 9, 38, 1, 1)  # Marker so the file runs on past the tail
    return t


def fx_chorus():
    """Held strings and an organ chord, chorus send full, no reverb."""
    t = Track()
    t.tempo(0, 120)
    t.program(0, 0, 48)
    t.program(0, 1, 16)
    for c in (0, 1):
        t.cc(0, c, 91, 0)
        t.cc(0, c, 93, 127)
    t.note(Q, 0, 60, 100, Q * 8)
    for k in (60, 64, 67):
        t.note(Q * 11, 1, k, 100, Q * 8)
    return t


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for name, fn in (("scale", scale), ("gm_tour", gm_tour), ("drums", drums),
                     ("expression", expression), ("poly", poly),
                     ("solo_pad", solo_pad), ("fx_reverb", fx_reverb), ("fx_chorus", fx_chorus)):
        write(os.path.join(out, name + ".mid"), fn())
        print(os.path.join(out, name + ".mid"))


if __name__ == "__main__":
    main()
