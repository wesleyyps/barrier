#!/bin/bash
APP_NAME="$1"
SCRIPT_PATH="$2"

APP_DIR="/Applications/$APP_NAME.app"
mkdir -p "$APP_DIR/Contents/MacOS"

cat << PLIST > "$APP_DIR/Contents/Info.plist"
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>runner</string>
    <key>CFBundleIconFile</key>
    <string></string>
    <key>CFBundleIdentifier</key>
    <string>com.barrier.${APP_NAME// /}</string>
    <key>CFBundleName</key>
    <string>$APP_NAME</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>LSUIElement</key>
    <true/>
</dict>
</plist>
PLIST

cat << RUNNER > "$APP_DIR/Contents/MacOS/runner"
#!/bin/bash
"$SCRIPT_PATH" "\$@"
RUNNER

chmod +x "$APP_DIR/Contents/MacOS/runner"
echo "Created $APP_DIR"
