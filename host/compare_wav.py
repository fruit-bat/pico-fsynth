#!/usr/bin/env python3
"""Compare two renders of the same MIDI file (e.g. pico-fsynth vs FluidSynth).

Usage: compare_wav.py ours.wav reference.wav [segment_seconds]

Prints overall level difference, correlation of the loudness envelopes,
spectral centroid (brightness) and, with segment_seconds, the segments whose
level differs the most (useful with gm_tour.mid, one program per 1.5 s).
"""
import sys
import wave

import numpy as np


def load(path):
    with wave.open(path, "rb") as w:
        rate = w.getframerate()
        ch = w.getnchannels()
        data = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float64) / 32768.0
    data = data.reshape(-1, ch)
    return rate, data.mean(axis=1), data


def db(x):
    return 20 * np.log10(np.maximum(x, 1e-9))


def rms_env(x, win):
    n = len(x) // win
    return np.sqrt(np.mean(x[: n * win].reshape(n, win) ** 2, axis=1))


def centroid(x, rate):
    # Average spectral centroid over 4096-sample frames with energy
    n = 4096
    frames = len(x) // n
    cs = []
    for i in range(frames):
        seg = x[i * n:(i + 1) * n] * np.hanning(n)
        mag = np.abs(np.fft.rfft(seg))
        if mag.sum() < 1e-3:
            continue
        freqs = np.fft.rfftfreq(n, 1 / rate)
        cs.append((mag * freqs).sum() / mag.sum())
    return np.mean(cs) if cs else 0.0


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    ra, a, a2 = load(sys.argv[1])
    rb, b, b2 = load(sys.argv[2])
    if ra != rb:
        sys.exit("sample rates differ")
    n = min(len(a), len(b))
    a, b = a[:n], b[:n]
    win = ra // 50  # 20 ms
    ea, eb = rms_env(a, win), rms_env(b, win)
    loud = (ea > 1e-4) | (eb > 1e-4)
    corr = np.corrcoef(db(ea[loud]), db(eb[loud]))[0, 1] if loud.sum() > 2 else float("nan")
    print(f"length      {len(a) / ra:7.2f} s (ours {len(a2) / ra:.2f}, ref {len(b2) / rb:.2f})")
    print(f"rms         ours {db(np.sqrt(np.mean(a ** 2))):6.1f} dB  ref {db(np.sqrt(np.mean(b ** 2))):6.1f} dB")
    print(f"peak        ours {db(np.abs(a2).max()):6.1f} dB  ref {db(np.abs(b2).max()):6.1f} dB")
    print(f"envelope    correlation of 20 ms dB levels: {corr:.3f}")
    print(f"centroid    ours {centroid(a, ra):6.0f} Hz  ref {centroid(b, rb):6.0f} Hz")
    # Stereo width: side/mid energy ratio
    for name, s in (("ours", a2), ("ref", b2)):
        if s.shape[1] == 2:
            mid = (s[:, 0] + s[:, 1]) / 2
            side = (s[:, 0] - s[:, 1]) / 2
            print(f"side/mid    {name} {db(np.sqrt(np.mean(side ** 2))) - db(np.sqrt(np.mean(mid ** 2))):6.1f} dB")

    if len(sys.argv) > 3:
        seg = int(float(sys.argv[3]) * ra)
        rows = []
        for i in range(n // seg):
            sa = a[i * seg:(i + 1) * seg]
            sb = b[i * seg:(i + 1) * seg]
            la, lb = db(np.sqrt(np.mean(sa ** 2))), db(np.sqrt(np.mean(sb ** 2)))
            rows.append((i, la, lb, la - lb, centroid(sa, ra), centroid(sb, rb)))
        diffs = np.array([r[3] for r in rows])
        print(f"segments    {len(rows)}: level diff median {np.median(diffs):+.1f} dB, "
              f"spread (10-90%) {np.percentile(diffs, 10):+.1f}..{np.percentile(diffs, 90):+.1f} dB")
        print("worst segments (index, ours dB, ref dB, diff, ours centroid, ref centroid):")
        for r in sorted(rows, key=lambda r: -abs(r[3] - np.median(diffs)))[:12]:
            print(f"  {r[0]:4d} {r[1]:7.1f} {r[2]:7.1f} {r[3]:+6.1f}  {r[4]:6.0f} {r[5]:6.0f}")


if __name__ == "__main__":
    main()
