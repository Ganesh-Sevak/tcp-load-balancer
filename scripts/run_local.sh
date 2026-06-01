#!/usr/bin/env bash
set -euo pipefail

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure

echo "Starting load balancer on 127.0.0.1:9000"
exec ./build/tcp-load-balancer --config config/backends.conf

