#!/bin/sh
cd "$(dirname "$0")/../.." || exit 1
mkdir -p build-asan
cd build-asan || exit 1
cmake -DCMAKE_BUILD_TYPE=Debug -DBARRIER_ENABLE_SANITIZERS=ON ..
make -j$(nproc)
