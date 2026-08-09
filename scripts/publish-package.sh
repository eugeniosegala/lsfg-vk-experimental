#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

version="$(tr -d '[:space:]' < VERSION)"
tag="v$version"
archive="out/lsfg-vk-$version-linux.tar.xz"
flatpak_archive="out/lsfg-vk-$version-flatpaks.tar.xz"
release_branch="$(git branch --show-current)"
source_commit="$(git rev-parse HEAD)"
release_remote="${LSFGVK_RELEASE_REMOTE:-experimental}"

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

if ! git remote get-url "$release_remote" >/dev/null 2>&1; then
    echo "Release remote '$release_remote' is not configured." >&2
    exit 1
fi

release_repository="$(git remote get-url "$release_remote")"
release_repository="${release_repository#git@github.com:}"
release_repository="${release_repository#https://github.com/}"
release_repository="${release_repository%.git}"
if [[ "$release_repository" != */* ]]; then
    echo "Could not determine a GitHub owner/repository from remote '$release_remote'." >&2
    exit 1
fi

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
scripts/package-flatpaks.sh "$flatpak_archive"
flatpak_checksum="$("${checksum_command[@]}" "$flatpak_archive" | awk '{print $1}')"
notes_file="$(mktemp "${TMPDIR:-/tmp}/lsfg-vk-release-notes.XXXXXX")"
cleanup() {
    rm -f "$notes_file"
}
trap cleanup EXIT

cat > "$notes_file" <<EOF
## Experimental Linux build

This is an experimental build of the lsfg-vk 2.x development line. Test it game by game and retain a known-good rollback path.

### Important limitations

- This build uses fixed 2x, 3x, or 4x frame-generation multipliers. It does not provide adaptive frame generation or an automatic multiplier.
- The 0x multiplier previously available in the 1.x line is not present in upstream lsfg-vk v2 and cannot be restored by this packaging layer.
- Lossless Scaling and its \`Lossless.dll\` must already be installed through Steam; this archive does not include or modify it.
- Flatpak runtime extensions for 23.08, 24.08, and 25.08 are included in \`$(basename "$flatpak_archive")\`. They use a dedicated experimental extension ID and can coexist with the public Flathub layer.

### Included

- Vulkan implicit layer: \`liblsfg-vk-layer.so\`
- CLI and Qt configuration UI
- Vulkan manifest and XDG desktop files

### Fixed

- Prevents undefined behaviour in Vulkan command submission when a submission has no timeline semaphore. Previously,
  lsfg-vk could access the end of an empty semaphore-value array.
- Avoids scheduling frame-generation GPU work while Gamescope has no generated-image slot available. The layer keeps
  real-frame history and timeline values aligned during this fallback, then resumes generation from the latest two real
  frames as soon as a non-blocking probe succeeds.

### Presentation diagnostics

- Adds opt-in timing diagnostics for frame scheduling, render-fence waits, swapchain image acquisition, GPU copy
  submissions, and generated/original image presentation.
- SteamOS traces identified generated-image acquisition as the operation dominating the observed Game Mode overlay
  stalls, with the total presentation duration closely tracking that wait.
- Diagnostics are disabled by default and do not perform timing or logging during normal runs.
- Adds an opt-in \`LSFGVK_PRESENT_ACQUIRE_TIMEOUT_MS\` recovery path. When Gamescope cannot provide an extra image before the
  configured timeout, lsfg-vk skips the remaining generated frames for that presentation, presents the original game
  frame, and switches subsequent attempts to non-blocking probes before scheduling more inference. This prevents the
  full timeout and discarded model work from recurring every frame, then resumes generation automatically when an image
  becomes available. The timeout remains disabled by default in the standalone engine; integrations can opt in per
  launched game.
- Aggregates expected non-blocking retry diagnostics. The first fallback reports whether backend work was scheduled or
  bypassed, and the recovery entry reports the number of bypassed frames without logging every retry.

With the isolated Decky LSFG-VK Experimental plugin, enable diagnostics with this Steam launch option:

\`\`\`bash
LSFGVK_PRESENT_ACQUIRE_TIMEOUT_MS=25 LSFGVK_PRESENT_DIAGNOSTICS=1 LSFGVK_PRESENT_DIAGNOSTICS_THRESHOLD_MS=25 ~/.local/bin/lsfg-vk-experimental %command%
\`\`\`

After reproducing the problem, extract the latest diagnostic entries with:

\`\`\`bash
grep -aF "lsfg-vk: present diagnostics:" ~/.steam/steam/logs/console-linux.txt | tail -n 400
\`\`\`

### Install

Download \`$(basename "$archive")\` and extract it to your local prefix:

\`\`\`bash
tar -xJf $(basename "$archive") -C ~/.local
\`\`\`

The host archive is for 64-bit Linux. Flatpak extensions are provided separately below.

### Flatpak extensions

Download and extract \`$(basename "$flatpak_archive")\`. It contains one self-contained experimental extension for each supported Flatpak runtime. Install the extension matching the application runtime, for example:

\`\`\`bash
flatpak install --user org.freedesktop.Platform.VulkanLayer.lsfgvkexperimental-24.08.flatpak
\`\`\`

- SHA-256: \`$flatpak_checksum\`

### Build details

- Source commit: \`$source_commit\`
- SHA-256: \`$checksum\`
- Upstream lineage: lsfg-vk \`2.0.0-dev28\`
EOF

git tag -a "$tag" -m "lsfg-vk experimental $version"
git push "$release_remote" "$release_branch"
git push "$release_remote" "$tag"

gh release create "$tag" "$archive" \
    "$flatpak_archive" \
    --repo "$release_repository" \
    --title "lsfg-vk Experimental $version" \
    --prerelease \
    --notes-file "$notes_file"

echo "Published: $tag"
