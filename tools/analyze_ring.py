#!/usr/bin/env python3
"""Look for structure in a raw Cam Link Pro DMA capture.

Usage: analyze_ring.py dump.bin [seglog.bin]

Reports byte statistics, known sync patterns (the reference driver's chunk
header and BT.656 SAV/EAV codes), the spacing between them, likely line
periods, and the DMA segment-length pattern from the seglog.
"""
import collections
import struct
import sys

PATTERNS = {
    "chunk hdr ff ff ff 00": b"\xff\xff\xff\x00",
    "bt656 ff 00 00": b"\xff\x00\x00",
}
KNOWN_PERIODS = [1920, 1936, 2640, 3840, 3856, 3872, 4096, 5280, 7680, 7744]


def find_all(data, pat, limit=200000):
    hits, i = [], data.find(pat)
    while i >= 0 and len(hits) < limit:
        hits.append(i)
        i = data.find(pat, i + 1)
    return hits


def hexdump(data, base, n=128):
    for off in range(0, min(n, len(data)), 16):
        chunk = data[off:off + 16]
        print(f"  {base + off:08x}: {chunk.hex(' ')}")


def byte_stats(data):
    counts = collections.Counter(data)
    zeros = counts.get(0, 0) / len(data)
    print(f"size {len(data)} bytes, {len(counts)} distinct byte values, "
          f"{zeros:.1%} zero")
    top = ", ".join(f"{b:02x}:{c / len(data):.1%}" for b, c in counts.most_common(10))
    print(f"most common: {top}")
    if len(counts) <= 2:
        print("=> (almost) constant data: the FPGA is not sending video")


def patterns(data):
    for name, pat in PATTERNS.items():
        hits = find_all(data, pat)
        print(f"\n[{name}] {len(hits)} hits")
        if not hits:
            continue
        gaps = collections.Counter(b - a for a, b in zip(hits, hits[1:]))
        print("  commonest gaps:", ", ".join(f"{g}x{c}" for g, c in gaps.most_common(8)))
        if pat == PATTERNS["bt656 ff 00 00"]:
            codes = collections.Counter(data[h + 3] for h in hits if h + 3 < len(data))
            print("  4th byte:", ", ".join(f"{c:02x}x{n}" for c, n in codes.most_common(8)))
        print("  first hit:")
        hexdump(data[hits[0]:], hits[0], 32)


def periods(data, window=1 << 20, samples=4096):
    """Fraction of bytes equal to the byte p later (sampled): line-structured
    video repeats strongly at its line pitch."""
    data = data[:window + 20000]
    if len(data) < 40000:
        return
    step = max(1, (len(data) - 20000) // samples)
    idx = range(0, len(data) - 20000, step)

    def score(p):
        return sum(data[i] == data[i + p] for i in idx) / len(idx)

    base = score(1)
    print(f"\nperiod scan (byte equality at distance p; p=1 gives {base:.2f})")
    best = sorted(((score(p), p) for p in range(16, 16384, 16)), reverse=True)[:10]
    print("  best multiples of 16:", ", ".join(f"{p}:{s:.2f}" for s, p in best))
    print("  known widths:", ", ".join(f"{p}:{score(p):.2f}" for p in KNOWN_PERIODS))


def seglog(path, seg_default=None):
    raw = open(path, "rb").read()
    pairs = list(struct.iter_unpack("<II", raw))
    if not pairs:
        print("\nseglog empty")
        return
    overruns = sum(1 for w0, _ in pairs if w0 == 0xFFFFFFFF)
    status = collections.Counter(w0 for w0, _ in pairs if w0 != 0xFFFFFFFF)
    lens = collections.Counter(w1 for w0, w1 in pairs if w0 != 0xFFFFFFFF)
    print(f"\nseglog: {len(pairs)} segments, {overruns} overrun markers")
    print("  writeback word0:", ", ".join(f"{s:08x}x{c}" for s, c in status.most_common(6)))
    print("  lengths:", ", ".join(f"{l}x{c}" for l, c in lens.most_common(10)))
    full = lens.most_common(1)[0][0]
    short = [i for i, (w0, w1) in enumerate(pairs) if w0 != 0xFFFFFFFF and w1 != full]
    if short:
        gaps = collections.Counter(b - a for a, b in zip(short, short[1:]))
        print(f"  {len(short)} non-{full} segments; spacing:",
              ", ".join(f"{g}x{c}" for g, c in gaps.most_common(6)))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    data = open(sys.argv[1], "rb").read()
    if not data:
        sys.exit("empty dump: no DMA data arrived")
    byte_stats(data)
    print("\nstart of dump:")
    hexdump(data, 0)
    patterns(data)
    periods(data)
    if len(sys.argv) > 2:
        seglog(sys.argv[2])


if __name__ == "__main__":
    main()
