#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
version="$(tr -d '[:space:]' < "$repo_root/VERSION")"
default_output="$repo_root/out/lsfg-vk-experimental-linux.tar.xz"
output_path=""
build_32_bit=true

usage() {
    cat <<'EOF'
Usage: scripts/package-local.sh [--64-bit-only] [output-path]

Build and verify a local Linux engine archive. --64-bit-only omits the 32-bit
layer and manifest for faster native 64-bit Deck/Steam Machine test builds.
EOF
}

while (($#)); do
    case "$1" in
        --64-bit-only)
            build_32_bit=false
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --*)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
        *)
            if [[ -n "$output_path" ]]; then
                echo "Only one output path may be specified" >&2
                exit 2
            fi
            output_path="$1"
            ;;
    esac
    shift
done
output_path="${output_path:-$default_output}"

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
    docker_64_only=0
    if [[ "$build_32_bit" == false ]]; then
        docker_64_only=1
    fi
    exec docker run --rm --platform linux/amd64 \
        -e LSFGVK_PACKAGE_64_ONLY="$docker_64_only" \
        -v "$repo_root:/workspace" \
        -w /workspace \
        ubuntu:22.04 \
        bash -lc '
            set -euo pipefail
            export DEBIAN_FRONTEND=noninteractive
            sed -i "s|http://|https://|g" /etc/apt/sources.list
            # Minimal Ubuntu images do not contain a CA bundle. APT still
            # verifies signed Ubuntu repository metadata during this one-time
            # TLS bootstrap; subsequent downloads use normal certificate checks.
            if [[ ! -s /etc/ssl/certs/ca-certificates.crt ]]; then
                apt-get -o Acquire::https::Verify-Peer=false update -qq
                apt-get -o Acquire::https::Verify-Peer=false install -y -qq ca-certificates
            fi
            apt-get update -qq
            apt-get install -y -qq \
                git curl llvm clang cmake ninja-build pkg-config g++-multilib \
                libvulkan-dev mesa-common-dev \
                qt6-base-dev qt6-base-dev-tools \
                qt6-tools-dev qt6-tools-dev-tools \
                qt6-declarative-dev qt6-declarative-dev-tools
            git clone --depth=1 -b vulkan-sdk-1.4.328 \
                https://github.com/KhronosGroup/Vulkan-Headers /tmp/vkh
            rm -rf /usr/include/vulkan /usr/include/vk_video
            cp -a /tmp/vkh/include/vulkan /tmp/vkh/include/vk_video /usr/include/
            package_args=()
            if [[ "${LSFGVK_PACKAGE_64_ONLY:-0}" == "1" ]]; then
                package_args+=(--64-bit-only)
            fi
            scripts/package-local.sh "${package_args[@]}" "/workspace/'"$output_relative"'"
        '
fi

for command in cmake ninja clang++ strings tar; do
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

build64_dir="$build_root/build64"
build32_dir="$build_root/build32"
install_dir="$build_root/target"
mkdir -p "$(dirname "$output_path")"

cmake -S "$repo_root" -B "$build64_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$install_dir" \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DLSFGVK_BUILD_VK_LAYER=ON \
    -DLSFGVK_BUILD_UI=ON \
    -DLSFGVK_BUILD_CLI=ON \
    -DLSFGVK_INSTALL_XDG_FILES=ON \
    -DLSFGVK_LAYER_LIBRARY_PATH="../../../lib/liblsfg-vk-layer.so"

cmake --build "$build64_dir" --target \
    lsfg-vk-config-tests lsfg-vk-profile-update-tests \
    lsfg-vk-runtime-transition-tests \
    lsfg-vk-presentation-policy-tests \
    lsfg-vk-adaptive-tests lsfg-vk-adaptive-matrix \
    lsfg-vk-pnext-chain-tests lsfg-vk-color-tests \
    lsfg-vk-hdr-color-math-tests
ctest --test-dir "$build64_dir" --output-on-failure
cmake --build "$build64_dir"
cmake --install "$build64_dir"

if [[ "$build_32_bit" == true ]]; then
    # A Vulkan layer is loaded into the application's process. Release archives
    # retain the second copy for genuine 32-bit games; local 64-bit-only builds
    # skip it to shorten the edit/test cycle.
    cmake -S "$repo_root" -B "$build32_dir" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$install_dir" \
        -DCMAKE_CXX_COMPILER=clang++ \
        -DCMAKE_CXX_FLAGS=-m32 \
        -DCMAKE_SHARED_LINKER_FLAGS=-m32 \
        -DCMAKE_INSTALL_LIBDIR=lib32 \
        -DBUILD_TESTING=OFF \
        -DLSFGVK_BUILD_VK_LAYER=ON \
        -DLSFGVK_BUILD_UI=OFF \
        -DLSFGVK_BUILD_CLI=OFF \
        -DLSFGVK_INSTALL_XDG_FILES=OFF \
        -DLSFGVK_LAYER_MANIFEST_SUFFIX=.x86 \
        -DLSFGVK_LAYER_LIBRARY_PATH="../../../lib32/liblsfg-vk-layer.so"

    cmake --build "$build32_dir" --target lsfg-vk-layer
    cmake --install "$build32_dir"
fi

required_paths=(
    "bin/lsfg-vk-cli" \
    "bin/lsfg-vk-ui" \
    "lib/liblsfg-vk-layer.so" \
    "share/vulkan/implicit_layer.d/VkLayer_LSFGVK_experimental_frame_generation.json"
)
if [[ "$build_32_bit" == true ]]; then
    required_paths+=(
        "lib32/liblsfg-vk-layer.so"
        "share/vulkan/implicit_layer.d/VkLayer_LSFGVK_experimental_frame_generation.x86.json"
    )
fi
for required_path in "${required_paths[@]}"; do
    if [[ ! -e "$install_dir/$required_path" ]]; then
        echo "Packaging failed: missing $required_path" >&2
        exit 1
    fi
done

verify_elf_class() {
    local path="$1"
    local expected="$2"
    local actual
    actual="$(od -An -t u1 -j 4 -N 1 "$path" | tr -d '[:space:]')"
    if [[ "$actual" != "$expected" ]]; then
        echo "Packaging failed: $path has unexpected ELF class byte $actual" >&2
        exit 1
    fi
}

verify_elf_class "$install_dir/lib/liblsfg-vk-layer.so" 2
if [[ "$build_32_bit" == true ]]; then
    verify_elf_class "$install_dir/lib32/liblsfg-vk-layer.so" 1
fi

manifest64="$install_dir/share/vulkan/implicit_layer.d/VkLayer_LSFGVK_experimental_frame_generation.json"
if ! grep -Fq '"library_arch": "64"' "$manifest64" ||
        ! grep -Fq '../../../lib/liblsfg-vk-layer.so' "$manifest64" ||
        ! grep -Fq '"name": "VK_LAYER_LSFGVK_experimental_frame_generation"' "$manifest64" ||
        ! grep -Fq '"ENABLE_LSFGVK_EXPERIMENTAL": "1"' "$manifest64" ||
        ! grep -Fq '"DISABLE_LSFGVK_EXPERIMENTAL": "1"' "$manifest64"; then
    echo "Packaging failed: 64-bit Vulkan manifest is incorrect" >&2
    exit 1
fi
if [[ "$build_32_bit" == true ]]; then
    manifest32="$install_dir/share/vulkan/implicit_layer.d/VkLayer_LSFGVK_experimental_frame_generation.x86.json"
    if ! grep -Fq '"library_arch": "32"' "$manifest32" ||
            ! grep -Fq '../../../lib32/liblsfg-vk-layer.so' "$manifest32" ||
            ! grep -Fq '"name": "VK_LAYER_LSFGVK_experimental_frame_generation"' "$manifest32" ||
            ! grep -Fq '"ENABLE_LSFGVK_EXPERIMENTAL": "1"' "$manifest32" ||
            ! grep -Fq '"DISABLE_LSFGVK_EXPERIMENTAL": "1"' "$manifest32"; then
        echo "Packaging failed: 32-bit Vulkan manifest is incorrect" >&2
        exit 1
    fi
fi

layer_binaries=("$install_dir/lib/liblsfg-vk-layer.so")
if [[ "$build_32_bit" == true ]]; then
    layer_binaries+=("$install_dir/lib32/liblsfg-vk-layer.so")
fi
for layer_binary in "${layer_binaries[@]}"; do
    if ! strings "$layer_binary" |
            grep -F "lsfg-vk: experimental layer active; identity=VK_LAYER_LSFGVK_experimental_frame_generation; build=$version" >/dev/null; then
        echo "Packaging failed: layer build identity diagnostic is missing from $layer_binary" >&2
        exit 1
    fi
done

tar -C "$install_dir" -cJf "$output_path" .

echo "Created and verified: $output_path"
echo "Version: $version"
echo "Architectures: $([[ "$build_32_bit" == true ]] && printf '64,32' || printf '64')"
