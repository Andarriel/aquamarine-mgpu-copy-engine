#!/bin/bash
# Fixes the botched install and installs the aquamarine-mgpu package properly.
# RUN WITH SUDO, from a TTY:   sudo bash ~/mgpu-7-fix-install.sh
#
# Two things went wrong:
#  1. /usr/lib/libaquamarine.so.0.15.0.orig (my backup copy) has the same SONAME as the real
#     library, so `ldconfig` happily pointed /usr/lib/libaquamarine.so.14 at the *backup*.
#     That is why the session silently went back to the slow CPU-readback path after a reboot.
#     The backup does not belong in a library directory; there is a copy in the project dir and
#     `pacman -S aquamarine` can always restore the distro build.
#  2. `makepkg -i --noconfirm` could not replace `aquamarine` because answering a conflict
#     prompt with --noconfirm means "no". This installs it with a real prompt.
set -e
if [ "$(id -u)" != 0 ]; then echo "run me with sudo"; exit 1; fi

PROJ=/home/andarriel/Experiments/mgpu
PKG=$(ls -t "$PROJ"/packaging/build/*.pkg.tar.zst 2>/dev/null | head -1)

echo "=== 1. removing the stray backup from /usr/lib (it confuses ldconfig)"
if [ -f /usr/lib/libaquamarine.so.0.15.0.orig ]; then
  if [ -f "$PROJ/build/libaquamarine.so.0.15.0.orig" ]; then
    rm -f /usr/lib/libaquamarine.so.0.15.0.orig
    echo "    removed (the project still has a copy at $PROJ/build/)"
  else
    mv /usr/lib/libaquamarine.so.0.15.0.orig "$PROJ/build/libaquamarine.so.0.15.0.orig"
    echo "    moved to $PROJ/build/"
  fi
else
  echo "    already gone"
fi

echo "=== 2. installing the package (say 'y' when it offers to remove aquamarine)"
if [ -z "$PKG" ]; then echo "no package found; run 'bash ~/mgpu-6-package.sh' first"; exit 1; fi
echo "    $PKG"
pacman -U "$PKG"

echo "=== 3. refreshing the linker cache"
ldconfig

echo
echo "=== result"
pacman -Q aquamarine-mgpu 2>&1 | sed 's/^/    /'
echo "    libaquamarine.so.14 -> $(readlink -f /usr/lib/libaquamarine.so.14)"
if strings "$(readlink -f /usr/lib/libaquamarine.so.14)" | grep -q "destination already up to date"; then
  echo "    copy engine + damage tracking: PRESENT"
else
  echo "    copy engine: MISSING -- something is wrong, run 'sudo pacman -S aquamarine' to go back"
fi
ls -la /usr/lib/libaquamarine.so* | sed 's/^/    /'
echo
echo "Now log out of Hyprland and back in (or reboot). To undo everything:"
echo "    sudo pacman -S aquamarine"
