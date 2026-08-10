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

tag_exists=false
if git rev-parse -q --verify "refs/tags/$tag" >/dev/null; then
    tag_commit="$(git rev-list -n 1 "$tag")"
    if [[ "$tag_commit" != "$source_commit" ]]; then
        echo "Tag $tag does not point at HEAD. Publish from its intended commit or bump VERSION." >&2
        exit 1
    fi
    tag_exists=true
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

- Adaptive Frame Generation is experimental and opt-in. This independent Vulkan-layer scheduler varies between zero
  and three generated frames per real frame to approach the configured average target, but it cannot reduce a native
  framerate already above that target, exceed 4x the current base rate, or provide the Windows Queue Target modes.
  Fixed 2x, 3x, and 4x modes remain available and unchanged.
- The 0x multiplier previously available in the 1.x line is not present in upstream lsfg-vk v2 and cannot be restored by this packaging layer.
- Lossless Scaling and its \`Lossless.dll\` must already be installed through Steam; this archive does not include or modify it.
- Flatpak runtime extensions for 23.08, 24.08, and 25.08 are included in \`$(basename "$flatpak_archive")\`. They use a dedicated experimental extension ID and can coexist with the public Flathub layer.

### Included

- Vulkan implicit layer: \`liblsfg-vk-layer.so\`
- CLI and Qt configuration UI
- Vulkan manifest and XDG desktop files

### Added

- Adds opt-in Adaptive Frame Generation through \`adaptive = true\` and \`target_fps = <FPS>\` profile settings.
- Adds \`adaptive_max_multiplier = 2|3|4\`, defaulting to 3x, so users can preserve image quality by allowing the
  displayed rate to undershoot the target instead of using a higher interpolation ratio when the real framerate falls.
- Uses a fractional output accumulator to vary the generated-frame count and uploads the corresponding interpolation
  timestamps before each inference pass. This supports non-integer average ratios such as 30 -> 55 or 50 -> 120.
- Skips interpolation below a 10 FPS base-rate safety floor and caps generation at the selected Adaptive limit, never
  exceeding three intermediate frames per real frame. Existing Fixed mode continues through its original path.
- Warms all three shared temporal-history slots with real frames before Adaptive generates its first output, avoiding
  startup inference from partially initialized history.
- Exposes Adaptive mode, target, and maximum multiplier in the standalone Qt configuration UI. Switching modes should
  be followed by a game restart so the swapchain is created with the intended capacity.

### Fixed

- Prevents undefined behaviour in Vulkan command submission when a submission has no timeline semaphore. Previously,
  lsfg-vk could access the end of an empty semaphore-value array.
- Avoids scheduling per-output frame-generation GPU work while Gamescope has no generated-image slot available. A
  shared history-only pre-pass still updates the model's temporal feature slots for every real frame, so counter-only
  bypass does not leave older temporal slots untouched.
- Resets Adaptive timing smoothing and fractional output credit when generated-image acquisition enters or leaves
  Gamescope backoff, so a compositor discontinuity is not carried into later scheduling decisions.
- After Gamescope first returns an image, keeps Adaptive output disabled for a three-real-frame history warm-up instead
  of treating one successful probe as a stable recovery. The acquired probe image is filled with the real frame and
  presented safely; renewed acquisition failure starts a fresh recovery cycle.
- Avoids multi-second recovery delays caused by repeatedly missing the image-release window with zero-timeout probes.
  After one second of fallback, the layer makes one bounded reacquisition attempt per second using the configured
  acquire timeout. It does not force the game to recreate its swapchain.

### Presentation diagnostics

- Adds opt-in timing diagnostics for frame scheduling, render-fence waits, swapchain image acquisition, GPU copy
  submissions, and generated/original image presentation.
- SteamOS traces identified generated-image acquisition as the operation dominating the observed Game Mode overlay
  stalls, with the total presentation duration closely tracking that wait.
- Diagnostics are disabled by default and do not perform timing or logging during normal runs.
- Adds an opt-in \`LSFGVK_PRESENT_ACQUIRE_TIMEOUT_MS\` recovery path. When Gamescope cannot provide an extra image before the
  configured timeout, lsfg-vk skips the remaining generated frames for that presentation, presents the original game
  frame, and switches subsequent attempts to non-blocking probes before scheduling more inference. After one second of
  fallback, it makes one bounded reacquisition attempt per second so it cannot remain phase-locked to an unavailable
  point in the presentation cycle. The timeout remains disabled by default in the standalone engine; integrations can
  opt in per launched game.
- Aggregates expected non-blocking retry diagnostics. The first fallback reports \`backend_work=scheduled\`; subsequent
  retries report \`backend_work=history-only\`. Periodic bounded attempts report
  \`acquire_mode=bounded-retry\`. Adaptive recovery emits \`generated-image-recovered\` followed by three
  \`history-warmup\` entries; startup and recovery warm-ups are identified separately. The recovery entry reports the
  number of bypassed output frames without logging every retry.

With the isolated Decky LSFG-VK Experimental plugin, enable diagnostics with this Steam launch option:

\`\`\`bash
LSFGVK_PRESENT_DIAGNOSTICS=1 LSFGVK_PRESENT_DIAGNOSTICS_THRESHOLD_MS=25 ~/.local/bin/lsfg-vk-experimental %command%
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

if [[ "$tag_exists" == false ]]; then
    git tag -a "$tag" -m "lsfg-vk experimental $version"
fi
git push "$release_remote" "$release_branch"
git push "$release_remote" "$tag"

gh release create "$tag" "$archive" \
    "$flatpak_archive" \
    --repo "$release_repository" \
    --title "lsfg-vk Experimental $version" \
    --prerelease \
    --notes-file "$notes_file"

echo "Published: $tag"
