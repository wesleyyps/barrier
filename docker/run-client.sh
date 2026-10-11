#!/bin/bash
# Script to run inside barrier-client container

export DISPLAY=:100
echo "Starting Xvfb..."
Xvfb :100 -screen 0 1920x1080x24 &
sleep 2

echo "Starting fluxbox..."
fluxbox &

echo "Starting x11vnc..."
env -u WAYLAND_DISPLAY x11vnc -display :100 -forever -shared -bg -rfbport 5902 -nopw -noshm -nocursorshape

echo "Starting dbus and Mutter..."
eval $(dbus-launch --sh-syntax)
export DBUS_SESSION_BUS_ADDRESS
export WAYLAND_DISPLAY=wayland-1
export XDG_RUNTIME_DIR=/tmp/runtime-developer
export XDG_SESSION_TYPE=wayland
export XDG_CURRENT_DESKTOP=GNOME
export QT_QPA_PLATFORM=wayland

mkdir -p $XDG_RUNTIME_DIR
chmod 0700 $XDG_RUNTIME_DIR

gnome-shell --wayland --nested &
MUTTER_PID=$!
sleep 10

echo "Starting Portals..."
/usr/libexec/xdg-desktop-portal &
/usr/libexec/xdg-desktop-portal-gnome &
sleep 2

echo "Building client..."
cd /workspace/build
cmake ..
make -j4 barrierc

echo "Starting barrier client..."
# We try to connect to the barrier-server which is on the host network (localhost:24800)
./bin/barrierc -f -n client --disable-crypto 127.0.0.1:24800 &
BARRIER_PID=$!

echo "Client running on VNC :5902. Use your VNC client to connect."
wait $BARRIER_PID
wait $MUTTER_PID
