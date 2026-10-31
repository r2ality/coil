#!/usr/bin/env sh
set -eu
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel "$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"
