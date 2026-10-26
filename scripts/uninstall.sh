#!/bin/sh
set -eu

if [ "$(id -u)" != 0 ]; then
    echo "run as root" >&2
    exit 1
fi
if dpkg-query -W -f='${Status}' touch-to-osc 2> /dev/null | grep -q "ok installed"; then
    echo "the touch-to-osc package is installed: remove it with apt remove touch-to-osc (apt purge also removes the config and the saved settings)" >&2
    exit 1
fi

systemctl disable --now touch-to-osc 2> /dev/null || true
rm -f /usr/local/lib/systemd/system/touch-to-osc.service /usr/local/bin/touch-to-osc /usr/local/bin/touch-to-osc-watch
rm -rf /usr/local/share/touch-to-osc /usr/local/share/doc/touch-to-osc
old=/etc/systemd/system/touch-to-osc.service
if [ -f "$old" ] && grep -q '^ExecStart=/usr/local/bin/touch-to-osc ' "$old"; then
    rm -f "$old"
fi
systemctl daemon-reload

if [ "${1:-}" = "--purge" ]; then
    rm -rf /etc/touch-to-osc /var/lib/touch-to-osc /var/lib/private/touch-to-osc
    echo "removed /etc/touch-to-osc and the saved settings"
else
    echo "kept /etc/touch-to-osc and the saved settings in /var/lib/touch-to-osc (run with --purge to remove them)"
fi
