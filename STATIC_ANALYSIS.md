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
