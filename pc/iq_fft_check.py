#!/usr/bin/env python3
"""Compare the firmware's spectrum against a double-precision reference FFT.

The firmware command `sp iqdump` prints one raw I/Q block plus the dB vector it
derives from that block:

    I (12345) serial_cli: IQ 512 6 512 1
    I (12345) serial_cli: IQDATA 1A2B...            # 2 * frames * frame_bytes hex chars
    I (12345) serial_cli: DB 1234 -22 ...           # 284 signed dB values in 1/8 dB units

Capture a session (for example `mise run monitor | tee capture.txt`, then type
`sp iqdump`), and run:

    python3 pc/iq_fft_check.py capture.txt

The reference repeats the firmware's pipeline in double precision: 24-bit
I/Q decode, Hann window, complex FFT, fftshift, power, optional positive-half
span, linear interpolation onto 284 columns, 10*log10. Any mismatch in the
decode, window, FFT or shift convention shows up as a large per-column error.

No third-party packages are required.
"""

from __future__ import annotations

import cmath
import math
import re
import sys

COLUMNS = 284
EPS = 1e-12
PREFIX = re.compile(r"^.*?serial_cli: ?")

# ---------------------------------------------------------------- radix-2 FFT


def fft(values: list[complex]) -> list[complex]:
    n = len(values)
    if n & (n - 1):
        raise ValueError(f"FFT size {n} is not a power of two")
    out = list(values)
    j = 0
    for i in range(1, n):
        bit = n >> 1
        while j & bit:
            j ^= bit
            bit >>= 1
        j |= bit
        if i < j:
            out[i], out[j] = out[j], out[i]

    size = 2
    while size <= n:
        half = size // 2
        w_step = cmath.exp(-2j * math.pi / size)
        for start in range(0, n, size):
            w = 1 + 0j
            for k in range(half):
                a = out[start + k]
                b = out[start + k + half] * w
                out[start + k] = a + b
                out[start + k + half] = a - b
                w *= w_step
        size *= 2
    return out


def db(power: float) -> float:
    return 10.0 * math.log10(power + EPS)


# ---------------------------------------------------------------- parse dump


def parse(text: str):
    header = None
    hexdata = ""
    db_q8: list[int] = []

    for raw in text.splitlines():
        line = PREFIX.sub("", raw).strip()
        if line.startswith("IQ "):
            parts = line.split()
            if len(parts) != 5:
                continue
            header = {
                "frames": int(parts[1]),
                "frame_bytes": int(parts[2]),
                "fft_size": int(parts[3]),
                "view_mode": int(parts[4]),
            }
            hexdata = ""
            db_q8 = []
        elif line.startswith("IQDATA "):
            hexdata += line.split(" ", 1)[1].strip()
        elif line.startswith("DB "):
            db_q8.extend(int(v) for v in line.split()[1:])

    if header is None:
        raise SystemExit("no IQ block found in the capture")
    if not hexdata or not db_q8:
        raise SystemExit("IQ block is incomplete (missing IQDATA or DB lines)")
    return header, bytes.fromhex(hexdata), db_q8


# ---------------------------------------------------------------- reference


def decode(raw: bytes, frames: int, frame_bytes: int) -> list[tuple[float, float]]:
    out = []
    for f in range(frames):
        base = f * frame_bytes
        i_raw = raw[base] | (raw[base + 1] << 8) | (raw[base + 2] << 16)
        q_raw = raw[base + 3] | (raw[base + 4] << 8) | (raw[base + 5] << 16)
        if i_raw & 0x800000:
            i_raw -= 1 << 24
        if q_raw & 0x800000:
            q_raw -= 1 << 24
        out.append((i_raw / 8388608.0, q_raw / 8388608.0))
    return out


def hann(n: int) -> list[float]:
    return [0.5 * (1.0 - math.cos(2.0 * math.pi * i / (n - 1))) for i in range(n)]


def reference_db(iq, fft_size: int, view_mode: int) -> list[float]:
    window = hann(fft_size)
    spec = fft([complex(i, q) * window[n] for n, (i, q) in enumerate(iq)])
    power = [abs(v) ** 2 for v in spec]
    half = fft_size // 2
    power = power[half:] + power[:half]  # fftshift
    bins = power[half:] if view_mode == 1 else power

    columns = []
    for px in range(COLUMNS):
        fbin = px * (len(bins) - 1) / (COLUMNS - 1)
        b = int(fbin)
        b = min(max(b, 0), len(bins) - 2)
        frac = fbin - b
        m = bins[b] * (1.0 - frac) + bins[b + 1] * frac
        columns.append(db(m))
    return columns


def main(argv: list[str]) -> int:
    if len(argv) > 1:
        text = open(argv[1], encoding="utf-8", errors="replace").read()
    else:
        text = sys.stdin.read()

    header, raw, db_q8 = parse(text)
    frames = header["frames"]
    if len(raw) < frames * header["frame_bytes"]:
        raise SystemExit(f"short IQ block: {len(raw)} bytes for {frames} frames")

    iq = decode(raw, frames, header["frame_bytes"])
    ref = reference_db(iq, header["fft_size"], header["view_mode"])
    dev = [v / 8.0 for v in db_q8]

    diffs = [r - d for r, d in zip(ref, dev)]
    worst = max(range(len(diffs)), key=lambda i: abs(diffs[i]))
    rms = math.sqrt(sum(d * d for d in diffs) / len(diffs))
    bias = sum(diffs) / len(diffs)

    print(f"frames={frames} frame_bytes={header['frame_bytes']} "
          f"fft={header['fft_size']} view={header['view_mode']} columns={len(diffs)}")
    print(f"error: max={abs(diffs[worst]):.3f} dB @col {worst} "
          f"(ref {ref[worst]:.1f} / dev {dev[worst]:.1f}), rms={rms:.3f} dB, bias={bias:+.3f} dB")
    print("verdict:", "OK" if abs(diffs[worst]) < 0.5 and rms < 0.2 else "MISMATCH")
    return 0 if abs(diffs[worst]) < 0.5 and rms < 0.2 else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
