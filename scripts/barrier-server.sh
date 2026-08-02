#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
if [ -f "$SCRIPT_DIR/.env" ]; then
    source "$SCRIPT_DIR/.env"
fi

NOHUP_BIN="/usr/bin/nohup"

BARRIER_GUI_BIN="${BARRIER_BIN_PATH}barrier"
BARRIER_SRV_BIN="${BARRIER_BIN_PATH}barriers"

killall -q barrier barriers barrierc || true

export ASAN_OPTIONS="$ASAN_OPTIONS"
export LSAN_OPTIONS="$LSAN_OPTIONS"
export UBSAN_OPTIONS="$UBSAN_OPTIONS"

$NOHUP_BIN $BARRIER_GUI_BIN >/dev/null 2>&1 &
$NOHUP_BIN $BARRIER_SRV_BIN -f -c $BARRIER_SRV_CONF &
