#!/bin/bash
# Installs the hardened v2 patched aquamarine. RUN WITH SUDO:  sudo bash ~/mgpu-1-install-v2.sh
set -e
SRC=/home/andarriel/Experiments/mgpu/build/libaquamarine.so.0.15.0
DST=/usr/lib/libaquamarine.so.0.15.0
if [ "$(id -u)" != 0 ]; then echo "run me with sudo"; exit 1; fi
if [ ! -f "$DST.orig" ]; then cp -a "$DST" "$DST.orig"; echo "backed up distro lib to $DST.orig"; fi
cp "$SRC" "$DST.new"
mv "$DST.new" "$DST"          # new inode, never overwrite a mapped file in place
chmod 755 "$DST"
echo "installed: $(md5sum $DST)"
echo "expected : 9479babadd07f14a86e932a875f2ccce"
