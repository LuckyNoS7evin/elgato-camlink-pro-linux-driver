#!/bin/bash
# Remove every DKMS-installed version of camlinkpro.
set -e
lsmod | grep -q '^camlinkpro ' && sudo rmmod camlinkpro
for v in $(dkms status camlinkpro | sed -n 's|^camlinkpro/\([^,:]*\).*|\1|p' | sort -u); do
	sudo dkms remove camlinkpro/"$v" --all
	sudo rm -rf /usr/src/camlinkpro-"$v"
done
dkms status camlinkpro
