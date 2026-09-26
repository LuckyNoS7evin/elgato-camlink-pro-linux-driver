#!/usr/bin/env python3
"""Render part of a raw capture as an image, via ffmpeg.

  render_raw.py dump.bin out.png --stride 1936 --height 1200
      grey "byte map": each row is `stride` raw bytes. Line-structured video
      shows up as a recognisable picture once the stride is right.

  render_raw.py dump.bin out.png --fmt yuyv --width 1920 --height 1080 \
      [--offset N] [--stride BYTES] [--skip BYTES]
      decode as a real pixel format (gray, yuyv, uyvy, nv12). --skip drops
      that many bytes at the start of every row (e.g. an 8-byte header).
"""
import argparse
import subprocess
import sys

BPP = {"gray": 1, "yuyv": 2, "uyvy": 2, "nv12": 1}
FFFMT = {"gray": "gray", "yuyv": "yuyv422", "uyvy": "uyvy422", "nv12": "nv12"}


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump")
    ap.add_argument("out")
    ap.add_argument("--fmt", choices=BPP, default="gray")
    ap.add_argument("--width", type=int, help="pixels (default: stride / bpp)")
    ap.add_argument("--height", type=int, default=1080)
    ap.add_argument("--stride", type=int, help="bytes per source row")
    ap.add_argument("--skip", type=int, default=0, help="bytes to drop per row")
    ap.add_argument("--offset", type=int, default=0)
    a = ap.parse_args()

    bpp = BPP[a.fmt]
    if not a.width and not a.stride:
        sys.exit("need --width or --stride")
    width = a.width or (a.stride - a.skip) // bpp
    stride = a.stride or width * bpp + a.skip
    rows = a.height * 3 // 2 if a.fmt == "nv12" else a.height

    with open(a.dump, "rb") as f:
        f.seek(a.offset)
        raw = f.read(stride * rows)
    if len(raw) < stride * rows:
        sys.exit(f"dump too short: need {stride * rows} bytes from offset {a.offset}")

    row_bytes = width * bpp
    img = b"".join(raw[r * stride + a.skip:r * stride + a.skip + row_bytes]
                   for r in range(rows))
    subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-f", "rawvideo",
                    "-pix_fmt", FFFMT[a.fmt], "-s", f"{width}x{a.height}",
                    "-i", "-", "-frames:v", "1", a.out], input=img, check=True)
    print(f"wrote {a.out}: {a.fmt} {width}x{a.height}, stride {stride}, "
          f"skip {a.skip}, offset {a.offset}")


if __name__ == "__main__":
    main()
