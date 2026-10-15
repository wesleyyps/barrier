#!/bin/bash
SOCK="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/barrier.sock"
if [ ! -S "$SOCK" ]; then
    SOCK="/tmp/barrier-$USER.sock"
fi
if [ ! -S "$SOCK" ]; then
    SOCK="/tmp/barrier-$(id -u).sock"
fi
if [ ! -S "$SOCK" ]; then
    echo "Barrier control socket not found: $SOCK" >&2
    exit 1
fi

echo "$@" | nc -U -w 1 "$SOCK" >/dev/null 2>&1 || true
