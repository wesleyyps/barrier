#!/bin/bash
# Script to run inside barrier-server container

export DISPLAY=:99
echo "Starting Xvfb..."
Xvfb :99 -screen 0 1024x768x24 &
sleep 2

echo "Starting fluxbox..."
fluxbox &

echo "Starting x11vnc..."
x11vnc -display :99 -forever -shared -bg -rfbport 5901 -nopw -nocursorshape

echo "Building server..."
cd /workspace/build
cmake ..
make -j4 barriers

echo "Creating barrier config..."
cat << 'IN_EOF' > /tmp/barrier.conf
section: screens
    server:
    client:
end

section: links
    server:
        right = client
    client:
        left = server
end
IN_EOF

echo "Starting barrier server..."
./bin/barriers -f -n server --disable-crypto -c /tmp/barrier.conf -a 0.0.0.0:24800 &
BARRIER_PID=$!

echo "Server running on VNC :5901. Use your VNC client to connect."
wait $BARRIER_PID
