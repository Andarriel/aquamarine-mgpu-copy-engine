#!/bin/bash
# Builds and installs `aquamarine-mgpu`: aquamarine 0.15.0 + the multi-GPU copy-engine patch,
# as a real pacman package so `pacman -Syu` can never silently restore the slow path.
#
# RUN WITHOUT SUDO, from a TTY (it will ask for your password when it needs pacman):
#     bash ~/mgpu-6-package.sh
#
# Best run from a text TTY rather than inside the Hyprland session you are replacing the
# library of. Pacman swaps files by unlink+create so a running session survives, but there is
# no reason to take the risk.
set -e
if [ "$(id -u)" = 0 ]; then echo "do NOT run me with sudo; makepkg refuses to run as root"; exit 1; fi

SRC=$HOME/Experiments/mgpu/packaging
BUILD=$HOME/Experiments/mgpu/packaging/build
LOG=$HOME/Experiments/mgpu/diag/package-$(date +%H%M%S).log
mkdir -p "$BUILD" "$(dirname "$LOG")"

# always package the current patch
cp "$HOME/Experiments/mgpu/aquamarine-mgpu-copy-engine.patch" "$SRC/"
cp "$SRC/PKGBUILD" "$SRC/aquamarine-mgpu-copy-engine.patch" "$BUILD/"

echo "=== building (log: $LOG)"
cd "$BUILD"
# -s installs missing build deps (vulkan-headers), -i installs the result, -f overwrites
# -s installs missing build deps, -f overwrites. NOT -i --noconfirm: replacing `aquamarine`
# needs a conflict prompt answered with "y", and --noconfirm answers the default, which is "no".
if makepkg -sf --noconfirm 2>&1 | tee "$LOG" | tail -5 && sudo pacman -U "$BUILD"/*.pkg.tar.zst && sudo ldconfig; then
  echo
  echo "=== installed:"
  pacman -Q aquamarine-mgpu aquamarine 2>&1 | sed 's/^/    /'
  echo "=== hyprland's dependency is satisfied by:"
  pacman -Qi aquamarine-mgpu | grep -E "^Provides|^Required By" | sed 's/^/    /'
  echo "=== library:"
  ls -la /usr/lib/libaquamarine.so.0.15.0 | sed 's/^/    /'
  if strings /usr/lib/libaquamarine.so.0.15.0 | grep -q "copier ready on"; then
    echo "    copy-engine path: PRESENT"
  else
    echo "    copy-engine path: MISSING -- something went wrong, run ~/mgpu-3-restore.sh"
  fi
  echo
  echo "    libaquamarine.so.14 -> $(readlink -f /usr/lib/libaquamarine.so.14)"
  echo
  echo "Log out of Hyprland and back in for the new library to be used."
  echo "To go back to the distro package: sudo pacman -S aquamarine"
else
  echo "BUILD FAILED, see $LOG (the installed library was not touched)"
  exit 1
fi
