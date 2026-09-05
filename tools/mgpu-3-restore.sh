#!/bin/bash
# Restores the distro aquamarine. RUN WITH SUDO:  sudo bash ~/mgpu-3-restore.sh
set -e
DST=/usr/lib/libaquamarine.so.0.15.0
if [ "$(id -u)" != 0 ]; then echo "run me with sudo"; exit 1; fi
BACKUP=/home/andarriel/Experiments/mgpu/build/libaquamarine.so.0.15.0.orig
[ -f "$DST.orig" ] && BACKUP="$DST.orig"
cp -a "$BACKUP" "$DST.new"
mv "$DST.new" "$DST"
rm -f "$DST.orig"   # must not linger: same SONAME, ldconfig would link against it
ldconfig
echo "restored: $(md5sum $DST)"
echo "so.14 -> $(readlink -f /usr/lib/libaquamarine.so.14)"
echo "(if the package is installed, 'pacman -S aquamarine' is the cleaner way back)"
echo "expected: ca04f8f5a1723854d8db27637c0471ac"
