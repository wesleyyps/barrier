# Code Scanning & Static Analysis

This document describes the static analysis and security scanning processes for the Barrier codebase using `clang-tidy` (local) and Coverity Scan (cloud).

---

## 1. Local Scanning with Clang-Tidy

Clang-Tidy provides immediate, local feedback on C++ code quality, undefined behavior, and modern best practices.

### Prerequisites

- **Linux (Debian/Ubuntu):**
  ```bash
  sudo apt install clang-tidy clang-tools
  ```
- **macOS:** Use the Homebrew LLVM installation (default Apple Clang does not include `clang-tidy`):
  ```bash
  brew install llvm
  ```

### Generating the Compilation Database

CMake exports `compile_commands.json` automatically during configuration:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
# compile_commands.json is now at build/compile_commands.json
```

**macOS only** — point CMake at the Homebrew LLVM before configuring:
```bash
export CMAKE_PREFIX_PATH="/opt/homebrew/opt/llvm"
. ./osx_environment.sh
```

### Running Clang-Tidy

**Linux:**
```bash
run-clang-tidy -p build/
```

**macOS** (use absolute path to Homebrew binary):
```bash
/opt/homebrew/opt/llvm/bin/run-clang-tidy \
  -clang-tidy-binary /opt/homebrew/opt/llvm/bin/clang-tidy \
  -p build/
```

---

## 2. Deep Analysis with Coverity Scan

Coverity Scan performs deep, inter-procedural static analysis (SAST) that detects complex memory leaks, data races, and concurrency vulnerabilities. Analysis runs on Coverity's cloud servers.

### One-time Setup

1. **Register:** Sign up at [scan.coverity.com](https://scan.coverity.com/) with your GitHub account and register the project.
2. **Download** the Coverity Build Tool (Linux 64-bit) from your dashboard and install it:
   ```bash
   tar xzvf coverity_tool.tgz -C ~/coverity
   echo 'export PATH="$HOME/coverity/bin:$PATH"' >> ~/.bashrc
   source ~/.bashrc
   ```
3. **Add credentials** to a `.env` file in the project root (already `.gitignore`d):
   ```bash
   COVERITY_EMAIL="your@email.com"
   COVERITY_TOKEN="your-project-token"
   COVERITY_PROJECT="youruser%2Fbarrier"
   COVERITY_PROJECT_ID="12345"
   ```

### Running a Scan

Everything is automated by the [`coverity-scan.sh`](coverity-scan.sh) script:

```bash
# Full clean build + upload (standard workflow)
./coverity-scan.sh

# Re-upload existing cov-int/ without rebuilding (e.g. after updating the modeling file)
./coverity-scan.sh --skip-build
```

The script will:
1. Load credentials from `.env`
2. Clean `build/` and `cov-int/`, then run a full CMake build intercepted by `cov-build`
3. Verify emission units were captured
4. Package `cov-int/` into `barrier-coverity.tgz`
5. Upload to Coverity Scan with git version/branch/commit as metadata
6. Print the dashboard URL on success

### Project Components

The following components are configured on the Coverity Scan dashboard to focus analysis on production code:

| Component | Pattern | Ignore |
|---|---|---|
| Third-party (googletest) | `.*/ext/googletest/.*` | ✅ |
| Tests | `.*/src/test/.*` | ✅ |
| Core Library | `.*/src/lib/.*` | ❌ |
| GUI | `.*/src/gui/.*` | ❌ |
| Command-line tools | `.*/src/cmd/.*` | ❌ |

### Modeling File

[`coverity-model.c`](coverity-model.c) teaches Coverity how to handle OpenSSL allocators (`EVP_PKEY`, `X509`, `RSA`, `BIGNUM`, `BIO`) and POSIX `fopen`/`fclose` so it doesn't generate false-positive null-dereference or resource-leak defects on those APIs.

To update the modeling file: edit `coverity-model.c` and upload it via the Coverity Scan dashboard under **Project Settings → Modeling File**, then re-submit the build with `./coverity-scan.sh --skip-build`.

Results are published at: **https://scan.coverity.com/projects/wesleyyps%2Fbarrier**

---

## 3. Dynamic Analysis & Fuzzing

Dynamic analysis catches vulnerabilities and undefined behaviors precisely at runtime, while Fuzzing bombards the parsing interfaces with malformed data to ensure stability.

### Enabling Sanitizers (ASan & UBSan)

AddressSanitizer (ASan) and UndefinedBehaviorSanitizer (UBSan) are supported natively. You must enable them during the CMake configuration step:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBARRIER_ENABLE_SANITIZERS=ON
make -C build
```

Once compiled, any execution (e.g., running `unittests` or `integtests`) will instantly crash and print a stack trace if it encounters an out-of-bounds memory access, use-after-free, or undefined behavior. Note that sanitizers introduce significant performance overhead and should not be used for Release builds.

### Fuzz Testing (libFuzzer)

Barrier parses binary network packets from clients. We use `libFuzzer` to fuzz the protocol parser (`ProtocolUtil`).

To compile the fuzzers, you **must use standard Clang** (e.g. on Linux or via a custom LLVM toolchain) and enable the fuzzing flag during CMake configuration:

> [!NOTE]
> **macOS Users:** Apple Clang omits the `libFuzzer` runtime library. If you compile with Apple Clang, the fuzzer targets will be automatically skipped to prevent linker errors. To fuzz on macOS, you must use a vanilla LLVM toolchain.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBARRIER_ENABLE_SANITIZERS=ON -DBARRIER_BUILD_FUZZERS=ON
make -C build
```

**Running the Fuzzer:**
The fuzzing executable is built to `build/bin/fuzz_protocol_util`. Execute it from your terminal:

```bash
./build/bin/fuzz_protocol_util
```

It will run indefinitely, throwing millions of mutated binary strings at the protocol parser. If it finds a crash, it will halt and print the exact input bytes that caused it.
