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
notes_version="2.0.0-dev28-experimental.25"
notes_previous_version="2.0.0-dev28-experimental.24"
notes_previous_tag="v$notes_previous_version"
notes_tag_pattern="v2.0.0-dev28-experimental.*"

if [[ "$version" != "$notes_version" ]]; then
    echo "Release notes still describe $notes_version. Update scripts/publish-package.sh for $version before publishing." >&2
    exit 1
fi

if ! git rev-parse -q --verify "refs/tags/$notes_previous_tag" >/dev/null; then
    echo "Release-note baseline tag $notes_previous_tag is missing." >&2
    exit 1
fi
if ! git merge-base --is-ancestor "$notes_previous_tag" HEAD; then
    echo "Release-note baseline $notes_previous_tag is not an ancestor of HEAD." >&2
    exit 1
fi

latest_previous_tag=""
while IFS= read -r candidate_tag; do
    if [[ "$candidate_tag" != "$tag" ]]; then
        latest_previous_tag="$candidate_tag"
        break
    fi
done < <(git tag --merged HEAD --list "$notes_tag_pattern" --sort=-version:refname)
if [[ "$latest_previous_tag" != "$notes_previous_tag" ]]; then
    echo "Release notes use $notes_previous_tag, but the latest prior tag is ${latest_previous_tag:-missing}. Update the baseline and change list before publishing." >&2
    exit 1
fi

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

## What's new since \`$notes_previous_version\`

This \`.25\` candidate adds explicit swapchain format/colour-space classification and supports the standard Gamescope HDR10/PQ and linear-scRGB paths without a user-facing HDR switch.

HDR10 input is converted from BT.2020/PQ into linear scRGB before the model and converted back after generation. Linear scRGB is passed directly to the model with its HDR constants enabled. Unvalidated HDR encodings stay on automatic real-frame passthrough instead of generating washed-out output.

### Changes introduced in this release

- Classifies the complete Vulkan format/colour-space pair instead of inferring HDR from a numeric format range.
- Supports both Gamescope packed HDR10 swapchain channel orders and FP16 linear scRGB.
- Decodes ST 2084 and converts BT.2020 to linear BT.709/scRGB before inference, then performs the inverse conversion for presentation.
- Keeps 8-bit and high-precision SDR model semantics distinct from HDR while selecting the correct storage-image format.
- Makes configuration reloads resilient to transient partial TOML writes instead of losing the update.
- Applies Frame Generation and Adaptive target, maximum multiplier, and Smooth Cadence changes safely in place. Changes that alter GPU resources or presentation shape are deferred; restart the game to guarantee those settings are rebuilt.
- Replaces indefinite device-wide waits during context replacement with bounded per-context retirement. A transient backend scheduling stall keeps native presentation active, then warms temporal history before frame generation resumes.
- Ships architecture-matched ELF64 and ELF32 Vulkan layers and manifests in the host archive and every supported Flatpak extension. The Vulkan loader selects the matching layer for each process; no launcher-side WoW64 workaround is required.
- Adds deterministic watched-configuration, profile-update, format-matrix, and HDR colour-math coverage. Adaptive scheduling, interpolation timestamps, generated-frame count, and Fixed 2x/3x/4x policy are unchanged.

### Important limitations

- Adaptive Frame Generation is experimental and opt-in. This independent Vulkan-layer scheduler varies between zero and three generated frames per real frame toward the configured average target. It cannot reduce a native framerate already above the target, exceed the selected 4x maximum, guarantee an unreachable target, or provide the Windows Queue Target modes.
- The 0x multiplier from lsfg-vk 1.x is not present in upstream v2. This fork provides a separate live synthesis switch that preserves the selected Fixed or Adaptive mode. Use \`DISABLE_LSFGVK_EXPERIMENTAL=1\` or remove the launch wrapper and restart the game when the experimental layer itself must be disabled.
- Higher interpolation ratios and lower real-frame rates can increase ghosting and input latency. Smooth Cadence can improve motion consistency but may lower real-frame cadence and responsiveness, so test it per game.
- HDR10 conversion adds one full-resolution decode dispatch per real frame and one encode dispatch per generated frame. It is correctness work, not a universal performance claim; measure the overhead on target hardware.
- HLG, Dolby Vision, and unvalidated wide-colour combinations intentionally use real-frame passthrough. A game still needs its own HDR renderer and an HDR-capable SteamOS/Gamescope session.
- Lossless Scaling and \`Lossless.dll\` must already be installed through Steam; neither release archive includes or modifies it.
- Generated-image timeout recovery remains inside the existing context. The layer does not force a game-owned swapchain recreation for a setting change or recovery decision.
- Flatpak extensions for 23.08, 24.08, and 25.08 are packaged separately in \`$(basename "$flatpak_archive")\` under a dedicated experimental ID that can coexist with the public Flathub layer.

### Included files

- 64-bit Vulkan implicit layer and manifest under \`lib/\` and \`share/vulkan/implicit_layer.d/\`
- 32-bit Vulkan implicit layer and manifest under \`lib32/\` and \`share/vulkan/implicit_layer.d/\`
- 64-bit CLI and Qt configuration UI
- XDG desktop files

### Adaptive configuration

Enable Adaptive mode through the Qt UI or a profile:

\`\`\`toml
adaptive = true
target_fps = 120
adaptive_max_multiplier = 3
adaptive_stable_cadence = false
frame_generation_enabled = true
\`\`\`

Frame Generation and the Adaptive target, maximum multiplier, and Smooth Cadence can update live when the current swapchain already has sufficient resources. Restart after switching between Fixed and Adaptive modes, changing the Fixed multiplier, GPU, Flow Scale, Performance Mode, pacing, DLL, or FP16 policy, or when increasing a setting beyond the resources created at startup.

### Optional diagnostics

Presentation diagnostics remain disabled by default. When enabled, slow Vulkan operations, fallback/recovery state, Adaptive ramp decisions, fast-cadence bursts, 2x gameplay-hitch recovery, and swapchain lifecycle events include a process-unique \`context=<ID>\` so concurrent or replacement contexts can be separated.

With the isolated Decky LSFG-VK Experimental plugin, enable diagnostics with this Steam launch option:

\`\`\`bash
LSFGVK_PRESENT_DIAGNOSTICS=1 LSFGVK_PRESENT_DIAGNOSTICS_THRESHOLD_MS=25 ~/.local/bin/lsfg-vk-experimental %command%
\`\`\`

After reproducing the problem, extract the latest diagnostic entries with:

\`\`\`bash
grep -aF "lsfg-vk: present diagnostics:" ~/.steam/steam/logs/console-linux.txt | tail -n 800
\`\`\`

See the [presentation-stall troubleshooting guide](https://github.com/eugeniosegala/lsfg-vk-experimental/blob/develop/docs/Troubleshooting.md#diagnosing-presentation-stalls) for recovery variables, event meanings, and focused filters. Disable diagnostics after collecting the trace.

### Install

Download \`$(basename "$archive")\` and extract it to your local prefix:

\`\`\`bash
tar -xJf $(basename "$archive") -C ~/.local
\`\`\`

The host archive includes 64-bit and 32-bit Vulkan layers; the CLI and Qt UI are 64-bit. Flatpak extensions are provided separately below.

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
