#!/bin/sh
set -eu

if [ "$(id -u)" != 0 ]; then
    echo "run as root" >&2
    exit 1
fi

systemctl disable --now touch-to-osc 2> /dev/null || true
rm -f /etc/systemd/system/touch-to-osc.service /usr/local/bin/touch-to-osc
systemctl daemon-reload

if [ "${1:-}" = "--purge" ]; then
    rm -rf /etc/touch-to-osc
    echo "removed /etc/touch-to-osc"
else
    echo "kept /etc/touch-to-osc (run with --purge to remove it)"
fi
