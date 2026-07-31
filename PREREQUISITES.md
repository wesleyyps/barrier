# Barrier — Build Prerequisites Reference Guide

This document lists the required compilers, build systems, libraries, and frameworks needed to compile **Barrier** (both CLI backends `barriers`/`barrierc` and the Qt-based GUI app).

---

## 🚀 Quick Diagnostic Tool

We have provided a cross-platform diagnostic script inside the repository root to check your environment and give you the exact copy-paste command to fix any missing dependencies:

```bash
# Run the diagnostic script (Linux & macOS)
./check_prereqs.py
```

---

## 🐧 Linux (Ubuntu / Debian Family)

To compile the codebase natively on Linux, you need the standard C++ compilation toolchain along with development headers for network, cryptography, mDNS, and X11 graphics.

### Core Prerequisites (CLI Binaries Only)
Required to compile the backend programs (`barriers` and `barrierc`):

- **Build Tools:** `cmake` (>= 3.8), `git`, `pkg-config`
- **Compiler:** C++14 compliant (GCC `g++` >= 5 or Clang >= 3.4)
- **Libraries & Headers:**
  - `libcurl4-openssl-dev` — Network file transfer services
  - `libssl-dev` — OpenSSL encryption backend
  - `libavahi-compat-libdnssd-dev` — Avahi mDNS/Bonjour compatibility library
  - `libx11-dev` — X11 Windowing system core client
  - `libxtst-dev` — X11 Record and Test extensions (for fake cursor/keyboard injection)
  - `libxrandr-dev` — X11 Resize and Rotate extension (screen resolution query)
  - `libxi-dev` — X11 Input extension (tracking cursor events)

**Installation Command:**
```bash
sudo apt-get install -y \
  build-essential \
  cmake \
  git \
  pkg-config \
  libcurl4-openssl-dev \
  libssl-dev \
  libavahi-compat-libdnssd-dev \
  libx11-dev \
  libxtst-dev \
  libxrandr-dev \
  libxi-dev
```

### GUI Prerequisites (Optional)
Required if you want to compile the Qt graphical configuration application (`barrier`):
- `qtbase5-dev` — Qt5 core development libraries
- `qttools5-dev` / `qttools5-dev-tools` — Qt5 development build utilities (`moc`, `uic`, `rcc`)

**Installation Command:**
```bash
sudo apt-get install -y qtbase5-dev qttools5-dev qttools5-dev-tools
```

---

## 🍏 macOS (via Homebrew)

Compiling on macOS requires Xcode compiler tools and libraries, which are easily installed using [Homebrew](https://brew.sh/).

### Core Prerequisites
- **Xcode Command Line Tools:** Compiler toolchain (Clang)
- **Homebrew packages:**
  - `cmake`
  - `openssl` (Homebrew puts this in `/opt/homebrew/opt/openssl` or `/usr/local/opt/openssl`)
  - `curl` (Usually system-provided, or custom from Homebrew)
  - *Bonjour/mDNS is built natively into macOS, so no extra libraries are needed.*

**Installation Commands:**
```bash
# 1. Install Xcode Command Line Tools
xcode-select --install

# 2. Install CMake and OpenSSL via Homebrew
brew install cmake openssl
```

### GUI Prerequisites (Optional)
Required if compiling the desktop configuration app on macOS:
- `qt@5` — Qt5 framework

**Installation Command:**
```brew install qt@5```

---

## ❖ Windows

Windows compilation requires Visual Studio, CMake, and packages managed through Microsoft's `vcpkg`.

### Core Prerequisites
- **Git for Windows:** VCS. [Download here](https://git-scm.com/download/win).
- **CMake:** Build system. [Download here](https://cmake.org/download/).
- **Visual Studio 2019/2022:** Community Edition or Build Tools. Ensure you select the **Desktop development with C++** workload in the installer.
- **Wix Toolset (Optional):** Required only if you want to bundle the build into a Windows `.msi` installer.

### Installing Libraries with vcpkg
We use `vcpkg` to build the required Curl and OpenSSL binaries for Windows:

```powershell
# Clone vcpkg and bootstrap it
git clone https://github.com/microsoft/vcpkg.git
.\vcpkg\bootstrap-vcpkg.bat

# Install Curl with SSL support and OpenSSL
.\vcpkg\vcpkg.exe install curl[ssl] openssl --triplet x64-windows
```

### GUI Prerequisites (Optional)
Required for the GUI app:
- **Qt5 SDK:** Download and install Qt 5.15 using the Qt Online Installer.
- **Environment Variable:** Ensure the environment variable `Qt5_DIR` points to your Qt installation directory (e.g. `C:\Qt\5.15.2\msvc2019_64\lib\cmake\Qt5`).
