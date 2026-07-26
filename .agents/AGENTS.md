# Barrier Agent Rules

## Mac OS

### Clang-Tidy & LLVM Toolchain
When running `clang-tidy` or working with LLVM tools on this project, be aware that the default Apple Clang binaries should not be overwritten. The user uses the Homebrew LLVM installation.

To ensure compilers and CMake can find the correct LLVM tools, you must export the following environment variables before invoking them:

**For compilers (if needed):**
```bash
export LDFLAGS="-L/opt/homebrew/opt/llvm/lib"
export CPPFLAGS="-I/opt/homebrew/opt/llvm/include"
```

**For CMake:**
```bash
export CMAKE_PREFIX_PATH="/opt/homebrew/opt/llvm"
```

When running `clang-tidy` directly, always use the absolute path to the Homebrew version:
`/opt/homebrew/opt/llvm/bin/clang-tidy` (or `/opt/homebrew/opt/llvm/bin/run-clang-tidy`).
