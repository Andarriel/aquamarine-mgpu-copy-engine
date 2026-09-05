#!/bin/bash
# Installs the hardened v2 patched aquamarine. RUN WITH SUDO:  sudo bash ~/mgpu-1-install-v2.sh
set -e
SRC=/home/andarriel/Experiments/mgpu/build/libaquamarine.so.0.15.0
DST=/usr/lib/libaquamarine.so.0.15.0
if [ "$(id -u)" != 0 ]; then echo "run me with sudo"; exit 1; fi
# NEVER keep a backup next to the library: it has the same SONAME, and ldconfig will happily
# point /usr/lib/libaquamarine.so.14 at the backup instead. Keep it in the project directory.
BACKUP=/home/andarriel/Experiments/mgpu/build/libaquamarine.so.0.15.0.orig
if [ ! -f "$BACKUP" ]; then cp -a "$DST" "$BACKUP"; echo "backed up distro lib to $BACKUP"; fi
if [ -f "$DST.orig" ]; then rm -f "$DST.orig"; echo "removed the dangerous $DST.orig"; fi
cp "$SRC" "$DST.new"
mv "$DST.new" "$DST"          # new inode, never overwrite a mapped file in place
chmod 755 "$DST"
ldconfig
echo "installed: $(md5sum $DST)"
echo "so.14 ->  $(readlink -f /usr/lib/libaquamarine.so.14)"
echo "expected : 9479babadd07f14a86e932a875f2ccce"
