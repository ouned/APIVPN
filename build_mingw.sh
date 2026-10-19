#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

# Locate a Wine launcher for the `--test` run below. On Debian/Ubuntu the
# `wine64` package only ships `/usr/bin/wine64-stable`; the bare `wine64` and
# `wine` names may be absent unless the `wine` metapackage is installed, so try
# each in turn. Override with WINE_BIN=<name-or-path> if needed.
find_wine() {
    local candidate
    for candidate in wine64 wine wine64-stable; do
        if command -v "$candidate" >/dev/null 2>&1; then
            echo "$candidate"
            return 0
        fi
    done
    # Some packaging layouts install only the raw loader under /usr/lib.
    candidate="$(find /usr/lib /opt -type f \( -name wine -o -name wine64 \) -path '*wine*' 2>/dev/null | sort -r | head -n 1)"
    if [ -n "$candidate" ]; then
        echo "$candidate"
        return 0
    fi
    return 1
}

echo "============================================================"
echo "Building APIVPN 32-bit (x86) Targets with MinGW-w64..."
echo "============================================================"
cmake -B build-mingw32 \
    -DCMAKE_TOOLCHAIN_FILE="$SCRIPT_DIR/cmake/toolchain-mingw32.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DAPIVPN_WERROR="${APIVPN_WERROR:-OFF}"
cmake --build build-mingw32 -j"$JOBS"

echo "============================================================"
echo "Building APIVPN 64-bit (x64) Targets with MinGW-w64..."
echo "============================================================"
cmake -B build-mingw64 \
    -DCMAKE_TOOLCHAIN_FILE="$SCRIPT_DIR/cmake/toolchain-mingw64.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DAPIVPN_WERROR="${APIVPN_WERROR:-OFF}"
cmake --build build-mingw64 -j"$JOBS"

echo "============================================================"
echo "All x86 and x64 binaries built successfully in ./bin/"
echo "============================================================"
ls -lh bin/*.exe bin/*.dll

if [ "${1:-}" = "--test" ]; then
    echo "============================================================"
    echo "Running Integration Test Suite under Wine..."
    echo "============================================================"
    # `|| true`: under `set -e` a failing find_wine inside the assignment would
    # abort the script before the error message below could be printed.
    WINE_BIN="${WINE_BIN:-$(find_wine || true)}"
    if [ -z "${WINE_BIN}" ]; then
        echo "error: no Wine launcher found (tried: wine64, wine64-stable, wine)" >&2
        exit 1
    fi
    echo "Using Wine launcher: ${WINE_BIN}"
    WINEDEBUG="${WINEDEBUG:--all}" "${WINE_BIN}" ./bin/integration_test_runner.exe
fi
