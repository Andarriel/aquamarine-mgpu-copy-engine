#!/bin/bash
# Restores the distro aquamarine. RUN WITH SUDO:  sudo bash ~/mgpu-3-restore.sh
set -e
DST=/usr/lib/libaquamarine.so.0.15.0
if [ "$(id -u)" != 0 ]; then echo "run me with sudo"; exit 1; fi
cp -a "$DST.orig" "$DST.new"
mv "$DST.new" "$DST"
echo "restored: $(md5sum $DST)"
echo "expected: ca04f8f5a1723854d8db27637c0471ac"
