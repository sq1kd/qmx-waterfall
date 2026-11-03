#!/usr/bin/env python3
"""Turn `sp shot` output into a PNG and report the row/column statistics.

Capture the serial console while the command runs, then:

    python3 pc/shot_check.py capture.txt [out.png] [--strip-rows N]

The dump is the exact RGB565 canvas the firmware pushes to the panel, so the
numbers below are about what the panel receives, with no camera involved.
`--strip-rows` is the height of the spectrum strip above the waterfall
(default 25 = SPEC_H; use 13 for the spectrum-off layout). No third-party
packages are required (zlib is in the standard library).
"""

from __future__ import annotations

import math
import re
import struct
import sys
import zlib

PREFIX = re.compile(r"^.*?serial_cli: ?")
# `mise run monitor | tee` (the workflow both tools document) colourises each
# line, so the log prefix arrives wrapped in SGR escapes.
ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


def parse(text: str):
    text = ANSI.sub("", text)
    header = {}
    data = ""
    for raw in text.splitlines():
        line = PREFIX.sub("", raw).strip()
        if line.startswith("SHOT "):
            parts = line.split()
            header = {
                "w": int(parts[1]),
                "h": int(parts[2]),
                "meta": " ".join(parts[3:]),
            }
            data = ""
        elif line.startswith("SHOTDATA "):
            data += line.split(" ", 1)[1].strip()
        elif line.startswith("SHOTEND"):
            break
    if not header or not data:
        raise SystemExit("no complete SHOT block in the capture")
    return header, bytes.fromhex(data)


def rgb565(v: int):
    r = (v >> 11) & 0x1F
    g = (v >> 5) & 0x3F
    b = v & 0x1F
    return (r * 255 // 31, g * 255 // 63, b * 255 // 31)


def write_png(path: str, w: int, h: int, rows, scale: int = 3):
    def chunk(tag: bytes, payload: bytes) -> bytes:
        body = struct.pack(">I", len(payload)) + tag + payload
        return body + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)

    raw = bytearray()
    for row in rows:
        line = bytearray()
        for px in row:
            line += bytes(px) * scale
        for _ in range(scale):  # PNG needs one filter byte per scanline
            raw += b"\x00" + bytes(line)
    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", w * scale, h * scale, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + chunk(b"IEND", b"")
    )
    with open(path, "wb") as f:
        f.write(png)


def autocorr(seq, lag: int) -> float:
    m = sum(seq) / len(seq)
    num = sum((seq[i] - m) * (seq[i + lag] - m) for i in range(len(seq) - lag))
    den = sum((v - m) ** 2 for v in seq)
    return num / den if den else 0.0


def ripple(seq, period: int) -> float:
    n = len(seq)
    m = sum(seq) / n
    re = sum((seq[i] - m) * math.cos(2 * math.pi * i / period) for i in range(n))
    im = sum((seq[i] - m) * math.sin(2 * math.pi * i / period) for i in range(n))
    return 2 * math.hypot(re, im) / n


def parse_args(argv):
    strip_rows = None
    positional = []
    args = list(argv[1:])
    i = 0
    while i < len(args):
        arg = args[i]
        if arg == "--strip-rows":
            i += 1
            if i >= len(args):
                raise SystemExit("--strip-rows expects a row count")
            value = args[i]
        elif arg.startswith("--strip-rows="):
            value = arg.split("=", 1)[1]
        elif arg.startswith("-"):
            raise SystemExit(f"unknown option {arg}")
        else:
            positional.append(arg)
            i += 1
            continue
        try:
            strip_rows = int(value)
        except ValueError:
            raise SystemExit(f"--strip-rows expects an integer, got {value!r}") from None
        i += 1
    return strip_rows, positional


def main(argv):
    strip_rows, positional = parse_args(argv)
    explicit_split = strip_rows is not None
    if strip_rows is None:
        strip_rows = 25
    text = open(positional[0], encoding="utf-8", errors="replace").read() if positional else sys.stdin.read()
    out_png = positional[1] if len(positional) > 1 else "shot_canvas.png"

    header, data = parse(text)
    w, h = header["w"], header["h"]
    need = w * h * 2
    if len(data) < need:
        raise SystemExit(f"short dump: {len(data)} bytes, need {need}")

    rows = []
    for y in range(h):
        row = []
        for x in range(w):
            v = data[(y * w + x) * 2] | (data[(y * w + x) * 2 + 1] << 8)
            row.append(rgb565(v))
        rows.append(row)

    luma = [[0.299 * r + 0.587 * g + 0.114 * b for r, g, b in row] for row in rows]

    if not 0 < strip_rows < h:
        raise SystemExit(f"--strip-rows {strip_rows} must be between 1 and {h - 1}")

    wf_top = strip_rows
    wf = [sum(luma[y]) / w for y in range(wf_top, h)]
    strip = [sum(luma[y]) / w for y in range(0, wf_top)]
    cols_wf = [sum(luma[y][x] for y in range(wf_top, h)) / (h - wf_top) for x in range(w)]
    cols_strip = [sum(luma[y][x] for y in range(0, wf_top)) / wf_top for x in range(w)]

    print(f"canvas {w}x{h}  {header['meta']}")
    source = "from --strip-rows" if explicit_split else "assuming SPEC_H; use --strip-rows to change"
    print(f"strip height {wf_top} rows (rows 0..{wf_top - 1}, {source})")
    print(f"strip rows 0..{wf_top - 1}: mean {sum(strip)/len(strip):.1f}  waterfall rows {wf_top}..{h-1}: mean {sum(wf)/len(wf):.1f}")
    print(f"waterfall row luma: min {min(wf):.1f} max {max(wf):.1f} std {math.sqrt(sum((v-sum(wf)/len(wf))**2 for v in wf)/len(wf)):.2f}")
    print("waterfall row autocorrelation 1..12: " + " ".join(f"{l}:{autocorr(wf,l):+.2f}" for l in range(1, 13)))
    print("waterfall ripple: " + " ".join(f"p{p}={ripple(wf,p):.2f}" for p in (2, 3, 4, 5, 6, 8, 12, 16, 24, 32)))
    print(f"waterfall column luma: left {sum(cols_wf[:w//2])/(w//2):.1f}  right {sum(cols_wf[w//2:])/(w-w//2):.1f}")
    print(f"strip column luma:     left {sum(cols_strip[:w//2])/(w//2):.1f}  right {sum(cols_strip[w//2:])/(w-w//2):.1f}")
    print("waterfall row profile (1 row per line):")
    for y in range(wf_top, h):
        print(f"  {y:2d} {luma[y] and sum(luma[y])/w:6.1f} " + "#" * int(sum(luma[y]) / w / 2))

    distinct = len({tuple(p) for row in rows for p in row})
    print(f"distinct colours: {distinct}")

    write_png(out_png, w, h, rows)
    print(f"wrote {out_png} ({w*3}x{h*3})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
