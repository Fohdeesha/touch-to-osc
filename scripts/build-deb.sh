#!/bin/sh
set -eu

here=$(cd "$(dirname "$0")/.." && pwd)
build=${BUILD_DIR:-$here/build-deb}

cmake -S "$here" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX=/usr -DTTO_WERROR=ON > /dev/null
ninja -C "$build"
"$build/tto_tests" --minimal
rm -f "$build"/touch-to-osc_*.deb
(cd "$build" && cpack -G DEB > /dev/null)
deb=$(ls "$build"/touch-to-osc_*.deb)
echo "built $deb"
echo "install with: sudo apt install $deb"
