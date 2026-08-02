#!/bin/bash

# Get script directory cross-platform, resolving symlinks
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
BARRIER_CLI_BIN="${BARRIER_BIN_PATH}barrierc"

OS="$(uname -s)"
if [ "$OS" = "Linux" ]; then
    killall -q barrier barriers barrierc || true
else
    killall -9 barrier barriers barrierc || true
fi

REMOTE_IP="$1:24800"

export ASAN_OPTIONS="$ASAN_OPTIONS"
export LSAN_OPTIONS="$LSAN_OPTIONS"
export UBSAN_OPTIONS="$UBSAN_OPTIONS"

OS="$(uname -s)"
if [ "$OS" = "Linux" ]; then
    if [ -z "$DISPLAY" ]; then
        export DISPLAY=:1
    fi
    if [ -z "$XAUTHORITY" ]; then
        export XAUTHORITY=$(ls /run/user/$(id -u)/gdm/Xauthority /run/user/$(id -u)/Xauthority $HOME/.Xauthority 2>/dev/null | head -n 1)
    fi
    $NOHUP_BIN $BARRIER_GUI_BIN >/dev/null 2>&1 &
    $NOHUP_BIN $BARRIER_CLI_BIN -f --display $DISPLAY -c $BARRIER_SRV_CONF "$REMOTE_IP" > /dev/null 2>&1 &
else
    # Simple execution with direct logging to the home directory to avoid any silent failures
    $NOHUP_BIN $BARRIER_GUI_BIN > /Users/wesley/barrier-gui.log 2>&1 &
    $NOHUP_BIN $BARRIER_CLI_BIN -f -c $BARRIER_SRV_CONF "$REMOTE_IP" > /Users/wesley/barrier-client.log 2>&1 &
fi
