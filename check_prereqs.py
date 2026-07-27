#!/usr/bin/env python3
import sys
import os
import shutil
import subprocess
import platform

# Color support check
def supports_color():
    plat = sys.platform
    supported_platform = plat != 'Pocket PC' and (plat != 'win32' or 'ANSICON' in os.environ)
    is_a_tty = hasattr(sys.stdout, 'isatty') and sys.stdout.isatty()
    return supported_platform and is_a_tty

COLOR = supports_color()

def print_status(label, success, info=""):
    if COLOR:
        icon = "\033[92m[PASS]\033[0m" if success else "\033[91m[FAIL]\033[0m"
        lbl = f"\033[1m{label:<35}\033[0m"
    else:
        icon = "[PASS]" if success else "[FAIL]"
        lbl = f"{label:<35}"
    
    if info:
        print(f" {icon} {lbl} {info}")
    else:
        print(f" {icon} {lbl}")

def heading(text):
    if COLOR:
        print(f"\n\033[95m\033[1m=== {text} ===\033[0m")
    else:
        print(f"\n=== {text} ===")

def run_cmd(cmd):
    try:
        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
        return True, res.stdout.decode().strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        return False, ""

def check_command(name, version_arg="--version"):
    path = shutil.which(name)
    if not path:
        return False, "Not found"
    ok, out = run_cmd([name, version_arg])
    ver = out.split('\n')[0] if ok else "Found"
    return True, f"({ver})"

def check_pkg_config(lib_name):
    ok, _ = run_cmd(["pkg-config", "--exists", lib_name])
    return ok

def check_linux():
    heading("Linux Dependency Diagnostics")
    
    # Toolchain checks
    cmake_ok, cmake_ver = check_command("cmake")
    print_status("CMake Build System", cmake_ok, cmake_ver)
    
    git_ok, git_ver = check_command("git")
    print_status("Git VCS", git_ok, git_ver)
    
    gxx_ok, gxx_ver = check_command("g++")
    clang_ok, clang_ver = check_command("clang++")
    compiler_ok = gxx_ok or clang_ok
    print_status("C++ Compiler (gcc or clang)", compiler_ok, gxx_ver if gxx_ok else clang_ver)
    
    pkg_ok, _ = check_command("pkg-config")
    print_status("pkg-config tool", pkg_ok)
    
    # Library checks via pkg-config
    missing_libs = []
    optional_missing = []
    
    libs = {
        "libcurl": ("Curl Library", "libcurl4-openssl-dev"),
        "openssl": ("OpenSSL Crypto", "libssl-dev"),
        "avahi-compat-libdns_sd": ("Bonjour/mDNS Compatibility", "libavahi-compat-libdnssd-dev"),
        "x11": ("X11 Foundation", "libx11-dev"),
        "xtst": ("X11 Test Extension", "libxtst-dev"),
        "xrandr": ("X11 RandR Extension", "libxrandr-dev"),
        "xi": ("X11 Input Extension", "libxi-dev")
    }
    
    for pkg, (label, deb_pkg) in libs.items():
        ok = check_pkg_config(pkg)
        print_status(label, ok)
        if not ok:
            missing_libs.append(deb_pkg)
            
    # GUI specific optional check
    qt_ok = check_pkg_config("Qt5Widgets")
    print_status("Qt5Widgets (GUI Optional)", qt_ok)
    if not qt_ok:
        optional_missing.extend(["qtbase5-dev", "qttools5-dev", "qttools5-dev-tools"])
        
    heading("Resolution Steps")
    if not cmake_ok or not git_ok or not compiler_ok or not pkg_ok or missing_libs:
        print("To fix the missing core dependencies, run this command:")
        install_cmd = "sudo apt-get install -y "
        deps = []
        if not cmake_ok: deps.append("cmake")
        if not git_ok: deps.append("git")
        if not compiler_ok: deps.append("build-essential")
        if not pkg_ok: deps.append("pkg-config")
        deps.extend(missing_libs)
        print(f"\n  \033[93m\033[1m{install_cmd}{' '.join(deps)}\033[0m\n")
    else:
        print("✅ \033[92mCore prerequisites are fully satisfied. You can compile the command line binaries!\033[0m")
        
    if optional_missing:
        print("\nTo build the GUI client as well, install Qt5 dependencies:")
        print(f"\n  \033[36m\033[1msudo apt-get install -y {' '.join(optional_missing)}\033[0m\n")
    else:
        print("✅ \033[92mGUI dependencies are satisfied.\033[0m")

def check_macos():
    heading("macOS Dependency Diagnostics")
    
    # Toolchain checks
    xcode_ok = os.path.exists("/usr/include") or os.path.exists("/Library/Developer/CommandLineTools")
    print_status("Xcode Command Line Tools", xcode_ok)
    
    cmake_ok, cmake_ver = check_command("cmake")
    print_status("CMake Build System", cmake_ok, cmake_ver)
    
    git_ok, git_ver = check_command("git")
    print_status("Git VCS", git_ok, git_ver)
    
    brew_ok, brew_ver = check_command("brew", "-v")
    print_status("Homebrew Package Manager", brew_ok, brew_ver)
    
    # Library checks via brew list
    missing_brew = []
    
    openssl_ok = check_pkg_config("openssl") or os.path.exists("/opt/homebrew/opt/openssl") or os.path.exists("/usr/local/opt/openssl")
    print_status("OpenSSL (via Homebrew)", openssl_ok)
    if not openssl_ok:
        missing_brew.append("openssl")
        
    # Apple has native mDNSResponder, libdns_sd.dylib is in the SDK.
    print_status("Bonjour/mDNS SDK", True, "(Native SDK)")
    
    # Qt5 check
    qt_ok = os.path.exists("/opt/homebrew/opt/qt@5") or os.path.exists("/usr/local/opt/qt@5")
    print_status("Qt5 Framework (GUI Optional)", qt_ok)
    if not qt_ok:
        missing_brew.append("qt@5")
        
    heading("Resolution Steps")
    if not xcode_ok:
        print("Install Xcode Command Line Tools first by running:")
        print("\n  \033[93m\033[1mxcode-select --install\033[0m\n")
        
    if not brew_ok:
        print("Homebrew is recommended to install libraries. Install it from https://brew.sh/")
    
    if brew_ok and missing_brew:
        print("To install the missing dependencies via Homebrew, run:")
        print(f"\n  \033[93m\033[1mbrew install {' '.join(missing_brew)}\033[0m\n")
    elif xcode_ok and cmake_ok and git_ok and not missing_brew:
        print("✅ \033[92mAll prerequisites are satisfied. Ready to compile!\033[0m")

def check_windows():
    heading("Windows Dependency Diagnostics")
    
    # Command checks
    cmake_ok, cmake_ver = check_command("cmake.exe")
    print_status("CMake Build System", cmake_ok, cmake_ver)
    
    git_ok, git_ver = check_command("git.exe")
    print_status("Git VCS", git_ok, git_ver)
    
    # Visual Studio / MSVC build tools search
    vswhere_ok, vswhere_path = check_command("vswhere.exe")
    vs_installed = False
    if vswhere_ok:
        # Check if vswhere outputs any VS installation
        try:
            vs_info = subprocess.check_output(["vswhere.exe", "-latest", "-property", "installationPath"]).decode().strip()
            if vs_info:
                vs_installed = True
        except Exception:
            pass
            
    print_status("Visual Studio 2019/2022 Build Tools", vs_installed)
    
    # Qt5 checks (commonly found in environment variables)
    qt_dir = os.environ.get("Qt5_DIR") or os.environ.get("QTDIR")
    print_status("Qt5 SDK Environment (GUI Optional)", bool(qt_dir), f"({qt_dir})" if qt_dir else "")
    
    heading("Resolution Steps")
    print("For Windows builds, the recommended setup is:")
    print("1. Install Git for Windows: https://git-scm.com/download/win")
    print("2. Install CMake: https://cmake.org/download/")
    print("3. Install Visual Studio 2022 Community with the 'C++ Desktop Development' workload.")
    print("4. Set up dependencies (OpenSSL, Curl) using vcpkg:")
    print("     git clone https://github.com/microsoft/vcpkg.git")
    print("     .\\vcpkg\\bootstrap-vcpkg.bat")
    print("     .\\vcpkg\\vcpkg.exe install curl[ssl] openssl --triplet x64-windows")
    if not qt_dir:
        print("5. Optional for GUI: Download Qt 5.15 from Qt Online Installer and set Qt5_DIR environment variable.")

def main():
    system = platform.system()
    if system == "Linux":
        check_linux()
    elif system == "Darwin":
        check_macos()
    elif system == "Windows":
        check_windows()
    else:
        print(f"Unsupported operating system: {system}")

if __name__ == "__main__":
    main()
