#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
if [ -f "$SCRIPT_DIR/.env" ]; then
    source "$SCRIPT_DIR/.env"
fi

NOHUP_BIN="/usr/bin/nohup"

BARRIER_GUI_BIN="${BARRIER_BIN_PATH}barrier"
BARRIER_CLI_BIN="${BARRIER_BIN_PATH}barrierc"

killall -q barrier barriers barrierc || true

REMOTE_IP="[$1]:24800"

export ASAN_OPTIONS="$ASAN_OPTIONS"
export LSAN_OPTIONS="$LSAN_OPTIONS"
export UBSAN_OPTIONS="$UBSAN_OPTIONS"

if [ -z "$DISPLAY" ]; then
    export DISPLAY=:1
fi
if [ -z "$XAUTHORITY" ]; then
    export XAUTHORITY=$(ls /run/user/$(id -u)/gdm/Xauthority /run/user/$(id -u)/Xauthority $HOME/.Xauthority 2>/dev/null | head -n 1)
fi

$NOHUP_BIN $BARRIER_GUI_BIN >/dev/null 2>&1 &
$NOHUP_BIN $BARRIER_CLI_BIN -f --display $DISPLAY -c $BARRIER_SRV_CONF "$REMOTE_IP"
