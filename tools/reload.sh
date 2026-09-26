#!/bin/bash
# (Re)load camlinkpro.ko, removing the old sc0710 driver first if present.
# Usage: tools/reload.sh [module params, e.g. pipe_ctrl=0x4180]
set -e
KO="$(dirname "$(readlink -f "$0")")/../driver/camlinkpro.ko"
PW_UNITS="pipewire.socket pipewire-pulse.socket wireplumber pipewire pipewire-pulse"

if lsmod | grep -q '^sc0710 '; then
	echo "Removing old sc0710 driver (pausing PipeWire, which holds its ALSA devices)..."
	systemctl --user stop $PW_UNITS || true
	if ! sudo rmmod sc0710; then
		echo "sc0710 still busy; killing remaining users of its device nodes"
		sudo fuser -k /dev/video[0-3] /dev/snd/* 2>/dev/null || true
		sleep 1
		sudo rmmod sc0710
	fi
	systemctl --user start $PW_UNITS || true
fi

if lsmod | grep -q '^camlinkpro '; then
	sudo rmmod camlinkpro
fi
sudo insmod "$KO" "$@"
sudo dmesg | grep -E 'camlinkpro' | tail -15
