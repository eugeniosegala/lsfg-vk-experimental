#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

version="$(tr -d '[:space:]' < VERSION)"
tag="v$version"
archive="out/lsfg-vk-$version-linux.tar.xz"
release_branch="$(git branch --show-current)"
source_commit="$(git rev-parse HEAD)"

if [[ "$release_branch" != "develop" ]]; then
    echo "Publish from develop; current branch is $release_branch." >&2
    exit 1
fi

if ! git diff --quiet || ! git diff --cached --quiet; then
    echo "Working tree has uncommitted changes. Commit or stash them before publishing." >&2
    exit 1
fi

if git rev-parse -q --verify "refs/tags/$tag" >/dev/null; then
    echo "Tag $tag already exists. Bump VERSION before publishing another release." >&2
    exit 1
fi

for command in gh git; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "Required command not found: $command" >&2
        exit 1
    fi
done

if command -v sha256sum >/dev/null 2>&1; then
    checksum_command=(sha256sum)
elif command -v shasum >/dev/null 2>&1; then
    checksum_command=(shasum -a 256)
else
    echo "Required command not found: sha256sum or shasum" >&2
    exit 1
fi

if ! gh auth status >/dev/null 2>&1; then
    echo "GitHub CLI is not authenticated. Run: gh auth login -h github.com" >&2
    exit 1
fi

scripts/package-local.sh "$archive"
checksum="$("${checksum_command[@]}" "$archive" | awk '{print $1}')"
notes_file="$(mktemp "${TMPDIR:-/tmp}/lsfg-vk-release-notes.XXXXXX")"
cleanup() {
    rm -f "$notes_file"
}
trap cleanup EXIT

cat > "$notes_file" <<EOF
## Experimental Linux build

This is an experimental build of the lsfg-vk 2.x development line. Test it game by game and retain a known-good rollback path.

### Included

- Vulkan implicit layer: \`liblsfg-vk-layer.so\`
- CLI and Qt configuration UI
- Vulkan manifest and XDG desktop files

### Install

Download \`$(basename "$archive")\` and extract it to your local prefix:

\`\`\`bash
tar -xJf $(basename "$archive") -C ~/.local
\`\`\`

The archive is for 64-bit Linux. It does not include Flatpak bundles.

### Build details

- Source commit: \`$source_commit\`
- SHA-256: \`$checksum\`
- Upstream lineage: lsfg-vk \`2.0.0-dev28\`
EOF

git tag -a "$tag" -m "lsfg-vk experimental $version"
git push origin "$release_branch"
git push origin "$tag"

gh release create "$tag" "$archive" \
    --title "lsfg-vk Experimental $version" \
    --prerelease \
    --notes-file "$notes_file"

echo "Published: $tag"
