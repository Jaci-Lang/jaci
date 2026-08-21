#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

# Default to Release with tests; users can override via extra args, e.g.
#   ./t/run_tests.sh -DCMAKE_BUILD_TYPE=Debug -j32
EXTRA_ARGS=()
if [[ $# -gt 0 ]]; then
    EXTRA_ARGS=("$@")
fi

cmake -S . -B build -DBUILD_TESTING=ON "${EXTRA_ARGS[@]}"
cmake --build build --target Luau.UnitTest Luau.Conformance t_native_interop_test -j

echo
echo "=== Luau.UnitTest ==="
./build/Luau.UnitTest

echo
echo "=== Luau.Conformance ==="
./build/Luau.Conformance

echo
echo "=== t_native_interop_test ==="
./build/t_native_interop_test "$ROOT/t/native_loop_bench.luau"
