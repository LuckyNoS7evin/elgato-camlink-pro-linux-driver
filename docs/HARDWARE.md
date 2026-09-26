# Cam Link Pro hardware notes

Status key: **confirmed** = seen on this card with this driver; **ref** = taken from
the stoth68000/sc0710 or Nakildias/sc0710 drivers (GPL-2.0) and not re-checked yet.

## Identity
- PCI `12ab:0710` (YUAN High-Tech), subsystem `1cfa:0011`. BAR0 1 MiB, BAR1 64 KiB, INTx pin B.
- BAR0 `0x0000` = `0x10ee7021` (Xilinx vendor ID) — ref.
- Lattice ECP5 LFE5U-85F, IDCODE `0x41113043` — confirmed, via the old driver's log.

## ECP5 bitstream (`/lib/firmware/sc0710/CAMLINKPRO.FWI.HEX`)
- Where it comes from: Elgato's Windows installer `Cam_Link_Pro_1.1.0.194.6.exe`. The file's SHA-256 is
  `ef3e792e8072c3a1d8b003ed4a92bcbedba6256719f866f2bfcbeed092f50b22`.
- Layout: a 16-byte header starting `00 11 22 33 …`, then 622035 bytes of payload. The payload is two halves stored
  in swapped order, and the stored first half is the longer one. The real bitstream is `(second ^ 0xA5)` followed by
  `(first ^ 0x5A)` — **confirmed offline**. The decoded stream begins `ff 00 "Lattice Semiconductor Corporation
  Bitstream" … Diamond 3.9.0.99.2`, with the sync word `ff ff bd b3` at offset 341.
- Load path: a Xilinx AXI Quad SPI controller at BAR0 `0x2000` talks to the ECP5 slave-SPI port. The sequence is
  REFRESH, ISC_ENABLE, ERASE, INIT_ADDRESS, then BITSTREAM_BURST (one chip-select cycle), check status, ISC_DISABLE,
  NOP. DONE is status bit 8 — ref.

## I2C / MCU
- Eight Xilinx AXI IIC cores at BAR0 `0x3000 + n*0x200`. Each needs a soft reset plus timing registers before use.
  Core 0 reaches the MCU at 7-bit address `0x32` — ref.
- Each input has a status block at subaddress `i*0x3c`: vtotal @4, htotal @6, height @8, width @10 (all LE) — ref.
- Input path register: `0x3b+i` = `0x10` (active) or `0x01` (idle). This was transcribed from the Windows driver's
  video-DMA start — ref.

## XDMA (BAR1) — standard Xilinx XDMA, see PG195
- Video uses C2H channel 0 @ `0x1000`; audio uses C2H channel 1 @ `0x1100`. Descriptor fetch is at `0x5080`.
- Descriptors use magic `0xad4b`. The C2H streaming writeback is 32 bytes per descriptor: word0 is status
  (`0x52b4…`), word1 is the byte count. Short counts mark the FPGA's end-of-packet flushes.

## Video pipeline (BAR0) — ref, not traced for this board
- `0xc8` height, `0xd8` scaler, `0xcc`/`0xdc` zeroed.
- `0xd0` control: `0x4100` base, bit 9 (`0x4300`) pulsed as a reset, bit 7 = native 4K (no half-scaler),
  bit 0 = GO (set last).

## Data stream — confirmed 2026-09-26 (capture a)
The stream is a sequence of chunks, one chunk per video line, back to back across DMA segments:
```
ff ff ff 00 | u16 LE: line << 3 | input | u16 LE: source width in pixels (0x0780) | payload | 8-byte pad
```
- **Native mode (`pipe_ctrl=0x4180`, the driver default):** 1920 px x 2 = 3840-byte payload, 3856-byte chunk, pad
  `ff` x 8.
- **Half mode (`0x4100`):** the FPGA halves the width, so 1080p arrives as 960x1080 (1920-byte payload,
  1936-byte chunk, pad `00` x 8). 1936 x 1080 x 50 = 104.5 MB/s, which is the rate every earlier attempt saw.
- The payload is packed **YUYV 4:2:2**, i.e. `Y U Y V`, with black = `10 80 10 80`.
- `line` runs from 1 to the frame height, strictly in order. A fresh GO starts exactly at line 1 of a frame.
- The Nakildias framer found no chunks because it wanted bytes 6..7 = `00 4f/6f`; on this card they hold the
  source width.
- DMA writeback word0 is `0x52b40000`, or `0x52b40001` on short segments. Short segments come about every 380–390
  segments. That is not frame-aligned (a frame is about 273 segments), and the chunk grid runs straight through them,
  so the framer can simply ignore segment boundaries.

## Experiment log
Each entry records: date, source signal, module params / pokes, then the result from `analyze_ring.py`.

- **a** (2026-09-26): Canon EOS on HDMI-1, 1080p50 (MCU: 1920x1080, total 2640x1125). Defaults (`pipe_ctrl=0x4100`,
  height 1080). Captured 32 MiB in 358 ms, no overruns. 16 complete frames, every chunk header valid, pads all zero.
  Rendered as 960x1080 YUYV it shows the camera's menu screen with correct colours.
  **First real image from this card on Linux.**
- **b** (2026-09-26): same source, `pipe_ctrl=0x4180`. Captured 64 MiB: 17404 chunks, all exactly 3856 bytes apart,
  lines 1..1080 in order, 16 frames. Rendered as 1920x1080 YUYV it is pixel-sharp at 1:1.
  **The native 1080p format is confirmed.**
- **v** (2026-09-26): first V4L2 run. `/dev/video0`, 1920x1080 YUYV, G_PARM 50/1. `v4l2-ctl --stream-mmap
  --stream-count=200` held a steady 50.00 fps; the framer saw 0 bad headers, 0 resyncs and 0 drops. `ffplay` played
  live with 0 frame drops, and the saved frame is clean.
  **Not yet tested:** the ECP5 upload after a cold boot (every run so far found DONE=1), a real live-view image rather
  than the camera menu, and OBS.
- **boot** (2026-09-26): DKMS install; the module autoloads at boot (5.7 s) and registers video0. After a normal
  shutdown and power-on the ECP5 was **still configured** (DONE=1). Standby power in the slot apparently keeps it
  alive, so only a PSU switch-off or unplug clears it. Added `fpga_reload=1` to force the upload path without a
  power cycle.
- **fpga_reload** (2026-09-26): `modprobe camlinkpro fpga_reload=1` wiped and reloaded the ECP5. The bitstream took
  2.5 s, the status went from `00200100` to `10200100` (DONE set; bit 28 newly set, meaning unknown, no error bits),
  and video came back at 50.00 fps. **The upload path is confirmed.**
