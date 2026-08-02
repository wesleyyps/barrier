#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
if [ -f "$SCRIPT_DIR/.env" ]; then
    source "$SCRIPT_DIR/.env"
fi

NOHUP_BIN="/usr/bin/nohup"

BARRIER_GUI_BIN="${BARRIER_BIN_PATH}barrier"
BARRIER_CLI_BIN="${BARRIER_BIN_PATH}barrierc"

pkill -x 'barrier|barriers|barrierc'

REMOTE_IP="[$1]:24800"

export ASAN_OPTIONS="$ASAN_OPTIONS"
export LSAN_OPTIONS="$LSAN_OPTIONS"
export UBSAN_OPTIONS="$UBSAN_OPTIONS"

$NOHUP_BIN $BARRIER_GUI_BIN >/dev/null 2>&1 &
$NOHUP_BIN $BARRIER_CLI_BIN -f --display :1 -c $BARRIER_SRV_CONF "$REMOTE_IP"
