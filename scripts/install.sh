#!/bin/sh
set -eu

if [ "$(id -u)" != 0 ]; then
    echo "run as root" >&2
    exit 1
fi
if dpkg-query -W -f='${Status}' touch-to-osc 2> /dev/null | grep -q "ok installed"; then
    echo "the touch-to-osc package is installed: install a newer package instead, or remove it first (apt remove touch-to-osc)" >&2
    exit 1
fi

here=$(cd "$(dirname "$0")/.." && pwd)
build=${BUILD_DIR:-$here/build}

cmake -S "$here" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX=/usr/local > /dev/null
ninja -C "$build"
"$build/tto_tests" --minimal
cmake --install "$build" > /dev/null

old=/etc/systemd/system/touch-to-osc.service
if [ -f "$old" ] && grep -q '^ExecStart=/usr/local/bin/touch-to-osc ' "$old"; then
    rm -f "$old"
fi
install -d -m 0755 /etc/touch-to-osc
if [ ! -f /etc/touch-to-osc/touch-to-osc.toml ]; then
    install -m 0644 "$here/config.example.toml" /etc/touch-to-osc/touch-to-osc.toml
    echo "wrote /etc/touch-to-osc/touch-to-osc.toml for one QDtech MPI1168 screen (usb 0712:0009); find yours with: touch-to-osc --list"
    echo "set match_usb (and match_port) in its [[device]] block and each receiver's IP in osc.allowed_ips"
fi
systemctl daemon-reload
checked=0
/usr/local/bin/touch-to-osc --check-config=/etc/touch-to-osc/touch-to-osc.toml || checked=$?
if [ "$checked" = 5 ]; then
    echo "its saved settings are refused (see above): when it starts it sets them aside and runs on the config file"
elif [ "$checked" = 2 ]; then
    echo "fix /etc/touch-to-osc/touch-to-osc.toml (see above) before starting it" >&2
elif [ "$checked" != 0 ]; then
    echo "checking /etc/touch-to-osc/touch-to-osc.toml did not finish (exit $checked, see above): try touch-to-osc --check-config before starting it" >&2
fi
if systemctl is-active --quiet touch-to-osc; then
    echo "the service is running and still runs the version from before: systemctl restart touch-to-osc (that ends any touches in progress)"
fi

echo "start with: systemctl enable --now touch-to-osc"
echo "remove with: $here/scripts/uninstall.sh (add --purge to remove the config and the saved settings too)"
