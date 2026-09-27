#!/usr/bin/env bash
# Build the web version on Linux or macOS with Docker: no local Emscripten needed.
#
#   Projects/emscripten/build_emscripten.sh          build into RunE/
#   Projects/emscripten/build_emscripten.sh serve    build, then serve RunE/ on http://localhost:8080
#
# Uses the same emsdk image as .github/workflows/web.yml. The build directory and Emscripten's
# cache live in Docker volumes, so rebuilds are incremental.
set -euo pipefail

IMAGE=emscripten/emsdk:6.0.10
ROOT=$(cd "$(dirname "$0")/../.." && pwd)

if [ ! -e "$ROOT/Run/Data" ]; then
    echo "Run/ is empty: fetch the demo data with 'git submodule update --init --recursive'" >&2
    exit 1
fi

docker volume create openfodder-ems-build >/dev/null
docker volume create openfodder-ems-cache >/dev/null

# Build as the calling user, so RunE/ isn't owned by root
docker run --rm -v openfodder-ems-build:/build -v openfodder-ems-cache:/cache "$IMAGE" \
    chown "$(id -u):$(id -g)" /build /cache
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -e EM_CACHE=/cache \
    -v "$ROOT":/src -v openfodder-ems-build:/build -v openfodder-ems-cache:/cache "$IMAGE" \
    bash -c 'set -e
        [ -e /cache/sysroot ] || cp -a /emsdk/upstream/emscripten/cache/. /cache/
        git config --global --add safe.directory /src
        emcmake cmake -S /src -B /build -DCMAKE_BUILD_TYPE=Release
        cmake --build /build -j"$(nproc)"'

echo "Built: $ROOT/RunE"
if [ "${1:-}" = "serve" ]; then
    echo "Serving on http://localhost:8080 (Ctrl+C to stop)"
    cd "$ROOT/RunE" && python3 -m http.server 8080
fi
