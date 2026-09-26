# Elgato Cam Link Pro — Linux driver (work in progress)

A from-scratch Linux driver for the Elgato Cam Link Pro PCIe card, which has 4× HDMI inputs
(PCI `12ab:0710`, subsystem `1cfa:0011`).

**Status:** HDMI-1 video up to 1920x1080 (YUYV) as a V4L2 device, usable in OBS, ffmpeg and v4l2-ctl.
Not done yet: audio, inputs 2–4, and 4K.

See `docs/HARDWARE.md` for what is known about the hardware.

## Requirements
- Kernel headers (`kernel-devel` on Fedora).
- `/lib/firmware/sc0710/CAMLINKPRO.FWI.HEX`, extracted from Elgato's Windows driver
  (`Cam_Link_Pro_1.1.0.194.6.exe`).

## Build and load
```sh
make -C driver
tools/reload.sh            # also unloads the old sc0710 driver if it is loaded
```

## Install (load at boot)
```sh
tools/dkms-install.sh      # installs dkms if needed; rebuilt for every new kernel
tools/dkms-uninstall.sh    # undo
```
After changing driver source, bump `PACKAGE_VERSION` in `dkms.conf` (or just re-run the install script, which
replaces the same version). For quick development cycles, `tools/reload.sh` loads the local build instead.

## Using it
```sh
v4l2-ctl --list-devices                      # "Elgato Cam Link Pro"
ffplay -f v4l2 -input_format yuyv422 /dev/videoN
```
In OBS, add a "Video Capture Device (V4L2)" source and pick "Elgato Cam Link Pro".

## Raw capture workflow
Everything under `/sys/kernel/debug/camlinkpro/` needs root.
```sh
D=/sys/kernel/debug/camlinkpro
sudo cat $D/info $D/status            # FPGA state, HDMI lock per input
echo 32 | sudo tee $D/capture         # stream HDMI-1, capture 32 MiB (blocks)
mkdir -p captures
sudo cat $D/dump > captures/a.bin; sudo cat $D/seglog > captures/a.seg
tools/analyze_ring.py captures/a.bin captures/a.seg
tools/render_raw.py captures/a.bin captures/a.png --stride 1936 --height 1200
```
Experiment knobs:
- Module parameters in `/sys/module/camlinkpro/parameters/`: `input`, `pipe_ctrl`, `pipe_height`, `ring_desc`,
  `ring_seg`.
- Writes such as `echo "bar0 0xd8 0x438" | sudo tee $D/poke`.

## License
GPL-2.0-or-later. The register knowledge comes partly from the GPL-2.0 sc0710 drivers by Steven Toth and the
Nakildias fork. The ECP5 bitstream is Elgato's and is not distributed here.
