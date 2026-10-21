#!/bin/sh
set -eu

if [ "$(id -u)" != 0 ]; then
    echo "run as root" >&2
    exit 1
fi

here=$(cd "$(dirname "$0")/.." && pwd)
build=${BUILD_DIR:-$here/build}

if [ ! -x "$build/touch-to-osc" ]; then
    cmake -S "$here" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX=/usr/local
    ninja -C "$build"
fi
"$build/tto_tests" > /dev/null
cmake --install "$build"

install -d -m 0755 /etc/touch-to-osc
if [ ! -f /etc/touch-to-osc/touch-to-osc.toml ]; then
    install -m 0644 "$here/config.example.toml" /etc/touch-to-osc/touch-to-osc.toml
    echo "wrote /etc/touch-to-osc/touch-to-osc.toml: add the receiver's address to osc.allowed_ips"
fi
/usr/local/bin/touch-to-osc --check-config=/etc/touch-to-osc/touch-to-osc.toml
install -m 0644 "$here/packaging/touch-to-osc.service" /etc/systemd/system/touch-to-osc.service
systemctl daemon-reload

echo "start with: systemctl enable --now touch-to-osc"
echo "remove with: $here/scripts/uninstall.sh (add --purge to remove the config too)"
