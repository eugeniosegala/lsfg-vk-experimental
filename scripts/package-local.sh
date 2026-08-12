#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
version="$(tr -d '[:space:]' < "$repo_root/VERSION")"
default_output="$repo_root/out/lsfg-vk-experimental-linux.tar.xz"
output_path="${1:-$default_output}"

if [[ "$output_path" != /* ]]; then
    output_path="$PWD/$output_path"
fi

if [[ -z "$version" ]]; then
    echo "VERSION must contain a release version." >&2
    exit 1
fi

if [[ "$(uname -s)" != "Linux" ]]; then
    if ! command -v docker >/dev/null 2>&1; then
        echo "Packaging needs Linux. Install Docker Desktop or run this script on Linux." >&2
        exit 1
    fi

    case "$output_path" in
        "$repo_root"/*)
            output_relative="${output_path#"$repo_root"/}"
            ;;
        *)
            echo "On non-Linux hosts, the output path must be inside this repository." >&2
            exit 1
            ;;
    esac

    echo "Using local linux/amd64 Docker packaging environment..."
    exec docker run --rm --platform linux/amd64 \
        -v "$repo_root:/workspace" \
        -w /workspace \
        ubuntu:22.04 \
        bash -lc '
            set -euo pipefail
            export DEBIAN_FRONTEND=noninteractive
            apt-get update -qq
            apt-get install -y -qq \
                git curl llvm clang cmake ninja-build pkg-config \
                libvulkan-dev mesa-common-dev \
                qt6-base-dev qt6-base-dev-tools \
                qt6-tools-dev qt6-tools-dev-tools \
                qt6-declarative-dev qt6-declarative-dev-tools
            git clone --depth=1 -b vulkan-sdk-1.4.328 \
                https://github.com/KhronosGroup/Vulkan-Headers /tmp/vkh
            rm -rf /usr/include/vulkan /usr/include/vk_video
            cp -a /tmp/vkh/include/vulkan /tmp/vkh/include/vk_video /usr/include/
            scripts/package-local.sh "/workspace/'"$output_relative"'"
        '
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

cmake --build "$build_dir" --target \
    lsfg-vk-adaptive-tests lsfg-vk-adaptive-matrix
ctest --test-dir "$build_dir" --output-on-failure
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
