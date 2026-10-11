#!/bin/sh
cd "$(dirname "$0")" || exit 1
# some environments have cmake v2 as 'cmake' and v3 as 'cmake3'
# check for cmake3 first then fallback to just cmake
B_CMAKE=`type cmake3 2>/dev/null`
if [ $? -eq 0 ]; then
    B_CMAKE=`echo "$B_CMAKE" | cut -d' ' -f3`
else
    B_CMAKE=cmake
fi
# default build configuration
B_BUILD_TYPE=${B_BUILD_TYPE:-Debug}
if [ "$(uname)" = "Darwin" ]; then
    # OSX needs a lot of extra help, poor thing
    # run the osx_environment.sh script to fix paths
    . ./osx_environment.sh
    B_CMAKE_FLAGS="-DCMAKE_OSX_SYSROOT=$(xcode-select --print-path)/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk -DCMAKE_OSX_DEPLOYMENT_TARGET=10.9 $B_CMAKE_FLAGS"
fi
# allow local customizations to build environment
[ -r ./build_env.sh ] && . ./build_env.sh

# Initialise Git submodules
git submodule update --init --recursive

B_CMAKE_FLAGS="-DCMAKE_BUILD_TYPE=$B_BUILD_TYPE $B_CMAKE_FLAGS"
rm -rf build
mkdir build || exit 1
cd build || exit 1
echo "Starting Barrier $B_BUILD_TYPE build..."
$B_CMAKE $B_CMAKE_FLAGS .. || exit 1
make || exit 1

if [ "$(uname)" = "Darwin" ]; then
    echo "Creating macOS application bundles for Barrier Server and Client..."
    
    create_mac_app() {
        APP_NAME="$1"
        SCRIPT_NAME="$2"
        APP_DIR="/Applications/$APP_NAME.app"
        
        echo "Creating $APP_DIR..."
        mkdir -p "$APP_DIR/Contents/MacOS"
        
        cat << PLIST > "$APP_DIR/Contents/Info.plist"
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>runner</string>
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
"$(pwd)/../scripts/$SCRIPT_NAME" "\$@"
RUNNER

        chmod +x "$APP_DIR/Contents/MacOS/runner"
    }

    create_mac_app "Barrier Server" "barrier-server.sh"
    create_mac_app "Barrier Client" "barrier-client-connect.sh"
    
    echo "macOS Application bundles installed in /Applications"
fi

echo "Build completed successfully"
