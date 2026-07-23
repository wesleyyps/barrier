# Code Scanning & Static Analysis

This document outlines the standard processes for performing static analysis and security scanning on the Barrier codebase using `clang-tidy` and Coverity Scan.

## 1. Local Scanning with Clang-Tidy

Clang-Tidy provides immediate, local feedback on C++ code quality, undefined behavior, and modern best practices.

### Prerequisites (macOS)
You must use the Homebrew installation of LLVM, as the default Apple Clang toolchain does not include the standalone `clang-tidy` binary.
```bash
brew install llvm
```

### Generating the Compilation Database
Clang-Tidy requires a `compile_commands.json` file to understand the project's include paths and build flags. To generate this, you must instruct CMake to export it while explicitly pointing to the Homebrew LLVM path.

Run the following command from the root of the repository:
```bash
export CMAKE_PREFIX_PATH="/opt/homebrew/opt/llvm"
. ./osx_environment.sh
cd build
cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_OSX_SYSROOT=$(xcode-select --print-path)/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk -DCMAKE_OSX_DEPLOYMENT_TARGET=10.9 ..
```
*Note: This will place a `compile_commands.json` file inside your `build` directory.*

### Running Clang-Tidy
Because you are using the Homebrew version of LLVM, you must use the absolute path to the binary to ensure it doesn't conflict with system tools.

**To scan a single file:**
```bash
/opt/homebrew/opt/llvm/bin/clang-tidy src/lib/server/ServerApp.cpp -p build/
```

**To scan the entire project concurrently:**
```bash
/opt/homebrew/opt/llvm/bin/run-clang-tidy -clang-tidy-binary /opt/homebrew/opt/llvm/bin/clang-tidy -p build/
```

---

## 2. Deep Analysis with Coverity Scan

Coverity Scan provides deep, inter-procedural static analysis (DAST/SAST) that looks for complex memory leaks, data races, and concurrency vulnerabilities. Because it requires massive computational power, the analysis is performed on Coverity's cloud servers.

### Registration
1. Sign up at [scan.coverity.com](https://scan.coverity.com/) using your GitHub account.
2. Register this repository (or your fork) as an Open Source project.
3. Download the **Coverity Build Tool** (`cov-build`) for macOS from the dashboard and extract it. Add its `bin` folder to your system `$PATH`.

### Intercepting the Build
Coverity works by "spying" on a clean compile. You must wrap the build command with `cov-build`.

1. **Clean the build directory** to ensure a full recompilation:
   ```bash
   rm -rf build && mkdir build
   ```
2. **Configure the environment**:
   ```bash
   . ./osx_environment.sh
   cd build
   cmake -DCMAKE_OSX_SYSROOT=$(xcode-select --print-path)/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk -DCMAKE_OSX_DEPLOYMENT_TARGET=10.9 ..
   ```
3. **Execute the wrapped build**:
   ```bash
   cov-build --dir cov-int make -j$(sysctl -n hw.ncpu)
   ```
   *This will generate a `cov-int` directory containing all the intercepted data.*

### Uploading the Results
1. Compress the intercepted data into a tarball:
   ```bash
   tar czvf barrier-coverity.tgz cov-int
   ```
2. Upload the `barrier-coverity.tgz` file via the Coverity Scan web dashboard. Alternatively, you can use the `curl` upload command provided on your dashboard along with your unique Project Token.

Within a few hours, the Coverity servers will process the upload and you will receive a dashboard report detailing any deep vulnerabilities found in the codebase.
