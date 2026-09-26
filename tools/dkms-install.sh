#!/bin/bash
# Install camlinkpro via DKMS: rebuilt for every new kernel, and loaded at
# boot automatically (udev matches the card's PCI ID).
set -e
ROOT="$(dirname "$(readlink -f "$0")")/.."
VER=$(sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' "$ROOT/dkms.conf")
SRC=/usr/src/camlinkpro-$VER

command -v dkms >/dev/null || sudo dnf install -y dkms
if [ ! -f /lib/firmware/sc0710/CAMLINKPRO.FWI.HEX ]; then
	echo "warning: /lib/firmware/sc0710/CAMLINKPRO.FWI.HEX missing; the card will not work after a cold boot" >&2
fi

# Replace any earlier install, of this or any other version.
for v in $(dkms status camlinkpro | sed -n 's|^camlinkpro/\([^,:]*\).*|\1|p' | sort -u); do
	sudo dkms remove camlinkpro/"$v" --all
	sudo rm -rf /usr/src/camlinkpro-"$v"
done
sudo rm -rf "$SRC"
sudo mkdir -p "$SRC/driver"
sudo cp "$ROOT/dkms.conf" "$SRC/"
sudo cp "$ROOT"/driver/{Kbuild,Makefile,*.c,*.h} "$SRC/driver/"

sudo dkms add camlinkpro/"$VER"
sudo dkms build camlinkpro/"$VER"
sudo dkms install camlinkpro/"$VER"

# Swap the running module for the installed one.
lsmod | grep -q '^sc0710 ' && echo "note: old sc0710 driver is loaded; run tools/reload.sh once or reboot" >&2
lsmod | grep -q '^camlinkpro ' && sudo rmmod camlinkpro
sudo modprobe camlinkpro
dkms status camlinkpro
modinfo -F filename camlinkpro
