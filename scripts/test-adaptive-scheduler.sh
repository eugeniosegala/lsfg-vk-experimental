#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

for command in cmake ctest; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "Required command not found: $command" >&2
        exit 1
    fi
done

build_root="$(mktemp -d "${TMPDIR:-/tmp}/lsfg-vk-adaptive-tests.XXXXXX")"
cleanup() {
    rm -rf "$build_root"
}
trap cleanup EXIT

generator="Unix Makefiles"
if command -v ninja >/dev/null 2>&1; then
    generator="Ninja"
fi

cmake -S "$repo_root" -B "$build_root" -G "$generator" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DLSFGVK_BUILD_VK_LAYER=OFF \
    -DLSFGVK_BUILD_UI=OFF \
    -DLSFGVK_BUILD_CLI=OFF \
    -DBUILD_TESTING=ON
cmake --build "$build_root"
ctest --test-dir "$build_root" --output-on-failure

echo "Adaptive scheduler tests and policy matrix passed."
