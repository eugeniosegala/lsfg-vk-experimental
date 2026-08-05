#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
version="$(tr -d '[:space:]' < "$repo_root/VERSION")"
default_output="$repo_root/out/lsfg-vk-experimental-linux.tar.xz"
output_path="${1:-$default_output}"

if [[ -z "$version" ]]; then
    echo "VERSION must contain a release version." >&2
    exit 1
fi

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "Packaging must run on Linux (or in a Linux development container)." >&2
    echo "The archive contains a Linux Vulkan layer and cannot be built on $(uname -s)." >&2
    exit 1
fi

for command in cmake ninja clang++ tar; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "Required command not found: $command" >&2
        exit 1
    fi
done

build_root="$(mktemp -d "${TMPDIR:-/tmp}/lsfg-vk-package.XXXXXX")"
cleanup() {
    rm -rf "$build_root"
}
trap cleanup EXIT

build_dir="$build_root/build"
install_dir="$build_root/target"
mkdir -p "$(dirname "$output_path")"

cmake -S "$repo_root" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$install_dir" \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DLSFGVK_BUILD_VK_LAYER=ON \
    -DLSFGVK_BUILD_UI=ON \
    -DLSFGVK_BUILD_CLI=ON \
    -DLSFGVK_INSTALL_XDG_FILES=ON \
    -DLSFGVK_LAYER_LIBRARY_PATH="../../../lib/liblsfg-vk-layer.so"

cmake --build "$build_dir"
cmake --install "$build_dir"

for required_path in \
    "bin/lsfg-vk-cli" \
    "bin/lsfg-vk-ui" \
    "lib/liblsfg-vk-layer.so" \
    "share/vulkan/implicit_layer.d/VkLayer_LSFGVK_frame_generation.json"; do
    if [[ ! -e "$install_dir/$required_path" ]]; then
        echo "Packaging failed: missing $required_path" >&2
        exit 1
    fi
done

tar -C "$install_dir" -cJf "$output_path" .

echo "Created and verified: $output_path"
echo "Version: $version"
