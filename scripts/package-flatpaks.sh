#!/usr/bin/env bash
# Build self-contained Flatpak Vulkan-layer bundles for the experimental fork.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
version="$(tr -d '[:space:]' < "$repo_root/VERSION")"
default_output="$repo_root/out/lsfg-vk-$version-flatpaks.tar.xz"
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
        echo "Flatpak packaging needs Linux. Install Docker Desktop or run this script on Linux." >&2
        exit 1
    fi

    case "$output_path" in
        "$repo_root"/*) output_relative="${output_path#"$repo_root"/}" ;;
        *)
            echo "On non-Linux hosts, the output path must be inside this repository." >&2
            exit 1
            ;;
    esac

    echo "Using local linux/amd64 Docker Flatpak packaging environment..."
    # Flatpak-builder starts a nested Bubblewrap sandbox. Docker's default seccomp
    # profile blocks the filter setup needed by that nested sandbox on Docker Desktop.
    exec docker run --rm --privileged --security-opt seccomp=unconfined --platform linux/amd64 \
        -v "lsfg-vk-flatpak-cache:/cache" \
        -e LSFGVK_DISABLE_BWRAP_SECCOMP=1 \
        -e LSFGVK_FLATPAK_CACHE_ROOT=/cache \
        -v "$repo_root:/workspace" \
        -w /workspace \
        ubuntu:24.04 \
        bash -lc '
            set -euo pipefail
            export DEBIAN_FRONTEND=noninteractive
            apt-get update -qq
            apt-get install -y -qq ca-certificates flatpak flatpak-builder xz-utils
            scripts/package-flatpaks.sh "/workspace/'"$output_relative"'"
        '
fi

for command in flatpak flatpak-builder tar; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "Required command not found: $command" >&2
        exit 1
    fi
done

# Docker Desktop's Linux VM may not expose CONFIG_SECCOMP_FILTER to nested
# Bubblewrap instances. This is only set by the privileged local Docker build
# above; normal Linux builds retain Flatpak's default sandboxing.
if [[ "${LSFGVK_DISABLE_BWRAP_SECCOMP:-0}" == "1" ]]; then
    export FLATPAK_BWRAP="$repo_root/scripts/bwrap-no-seccomp.sh"
fi

build_root="$(mktemp -d "${TMPDIR:-/tmp}/lsfg-vk-flatpak-package.XXXXXX")"
cleanup() {
    rm -rf "$build_root"
}
trap cleanup EXIT

cache_root="${LSFGVK_FLATPAK_CACHE_ROOT:-$build_root}"
export HOME="$cache_root/home"
export XDG_CACHE_HOME="$cache_root/cache"
export XDG_CONFIG_HOME="$cache_root/config"
export XDG_DATA_HOME="$cache_root/data"

mkdir -p "$HOME" "$XDG_CACHE_HOME" "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" "$(dirname "$output_path")"
flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo

bundle_dir="$build_root/bundles"
repo_dir="$build_root/repo"
mkdir -p "$bundle_dir" "$repo_dir"

extension_id="org.freedesktop.Platform.VulkanLayer.lsfgvkexperimental"
for runtime_version in 23.08 24.08 25.08; do
    manifest="$repo_root/dist/flatpak/lsfg-vk-layer/$extension_id"_"$runtime_version.yml"
    build_dir="$build_root/build-$runtime_version"
    bundle="$bundle_dir/$extension_id-$runtime_version.flatpak"

    if [[ ! -f "$manifest" ]]; then
        echo "Missing Flatpak manifest: $manifest" >&2
        exit 1
    fi

    echo "Building experimental Flatpak runtime extension $runtime_version..."
    flatpak-builder --force-clean --user --install-deps-from=flathub \
        --state-dir="$build_root/state-$runtime_version" \
        --repo="$repo_dir" "$build_dir" "$manifest"
    flatpak build-bundle "$repo_dir" "$bundle" "$extension_id" "$runtime_version" --runtime

    if [[ ! -s "$bundle" ]]; then
        echo "Flatpak packaging failed: missing $bundle" >&2
        exit 1
    fi
done

# The source directory is mounted into Flatpak-builder as a local source. Some
# builder versions remove ignored output directories while cleaning the source
# staging area, so recreate the destination immediately before writing it.
mkdir -p "$(dirname "$output_path")"
tar -C "$bundle_dir" -cJf "$output_path" .
echo "Created and verified: $output_path"
echo "Version: $version"
