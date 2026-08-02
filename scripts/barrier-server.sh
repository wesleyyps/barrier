#!/bin/bash

SOURCE="${BASH_SOURCE[0]}"
while [ -h "$SOURCE" ]; do
  DIR="$( cd -P "$( dirname "$SOURCE" )" >/dev/null 2>&1 && pwd )"
  SOURCE="$(readlink "$SOURCE")"
  [[ $SOURCE != /* ]] && SOURCE="$DIR/$SOURCE"
done
SCRIPT_DIR="$( cd -P "$( dirname "$SOURCE" )" >/dev/null 2>&1 && pwd )"

if [ -f "$SCRIPT_DIR/.env" ]; then
    source "$SCRIPT_DIR/.env"
fi

NOHUP_BIN="/usr/bin/nohup"

BARRIER_GUI_BIN="${BARRIER_BIN_PATH}barrier"
BARRIER_SRV_BIN="${BARRIER_BIN_PATH}barriers"

OS="$(uname -s)"
if [ "$OS" = "Linux" ]; then
    killall -q barrier barriers barrierc || true
else
    killall -9 barrier barriers barrierc || true
fi

export ASAN_OPTIONS="$ASAN_OPTIONS"
export LSAN_OPTIONS="$LSAN_OPTIONS"
export UBSAN_OPTIONS="$UBSAN_OPTIONS"

$NOHUP_BIN $BARRIER_GUI_BIN >/dev/null 2>&1 &
$NOHUP_BIN $BARRIER_SRV_BIN -f -c $BARRIER_SRV_CONF >/dev/null 2>&1 &
