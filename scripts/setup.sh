#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"

if [ ! -f "$SCRIPT_DIR/.env" ]; then
    echo "Creating .env from .env-sample..."
    cp "$SCRIPT_DIR/.env-sample" "$SCRIPT_DIR/.env"
fi

echo "Creating symlinks in /usr/local/bin (may prompt for password)..."
sudo rm -f /usr/local/bin/barrier-server.sh /usr/local/bin/barrier-client-connect.sh
sudo ln -sf "$SCRIPT_DIR/barrier-server.sh" /usr/local/bin/barrier-server.sh
sudo ln -sf "$SCRIPT_DIR/barrier-client-connect.sh" /usr/local/bin/barrier-client-connect.sh

echo "Setup complete!"
