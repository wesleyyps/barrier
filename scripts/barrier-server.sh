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

if [ "$OS" = "Linux" ]; then
    if command -v systemctl >/dev/null 2>&1; then
        eval "$(systemctl --user show-environment 2>/dev/null | grep -E '^(DISPLAY|WAYLAND_DISPLAY|XAUTHORITY|DBUS_SESSION_BUS_ADDRESS|XDG_RUNTIME_DIR)=')"
    fi
    if [ -z "$XDG_RUNTIME_DIR" ]; then
        export XDG_RUNTIME_DIR="/run/user/$(id -u)"
    fi
    if [ -z "$DBUS_SESSION_BUS_ADDRESS" ]; then
        export DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$(id -u)/bus"
    fi
    if [ -z "$WAYLAND_DISPLAY" ] && [ -S "$XDG_RUNTIME_DIR/wayland-0" ]; then
        export WAYLAND_DISPLAY="wayland-0"
    fi
    if [ -z "$DISPLAY" ]; then
        if [ -e "/tmp/.X11-unix/X0" ] && [ -n "$(ls /run/user/$(id -u)/.mutter-Xwaylandauth* 2>/dev/null)" ]; then
            export DISPLAY=:0
        elif [ -e "/tmp/.X11-unix/X1" ]; then
            export DISPLAY=:1
        else
            export DISPLAY=:0
        fi
    fi
    if [ -z "$XAUTHORITY" ]; then
        export XAUTHORITY=$(ls -t /run/user/$(id -u)/.mutter-Xwaylandauth* /run/user/$(id -u)/xauth* /run/user/$(id -u)/gdm/Xauthority /run/user/$(id -u)/Xauthority $HOME/.Xauthority 2>/dev/null | head -n 1)
    fi
    if command -v setsid >/dev/null 2>&1; then
        setsid -f "$BARRIER_SRV_BIN" -f -c "$BARRIER_SRV_CONF" </dev/null >/dev/null 2>&1
        setsid -f "$BARRIER_GUI_BIN" </dev/null >/dev/null 2>&1
    else
        $NOHUP_BIN "$BARRIER_SRV_BIN" -f -c "$BARRIER_SRV_CONF" </dev/null >/dev/null 2>&1 &
        $NOHUP_BIN "$BARRIER_GUI_BIN" </dev/null >/dev/null 2>&1 &
    fi
else
    if [ -n "$SSH_CONNECTION" ]; then
        open -a "Barrier Server"
    else
        exec $BARRIER_SRV_BIN -f -c $BARRIER_SRV_CONF >/dev/null 2>&1
    fi
    $NOHUP_BIN "$BARRIER_GUI_BIN" </dev/null >/dev/null 2>&1 &
fi
