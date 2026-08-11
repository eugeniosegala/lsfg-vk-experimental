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

## This release: Adaptive Frame Generation and SteamOS recovery

Version \`v2.0.0-dev28-experimental.9\` introduced the initial target-driven Adaptive scheduler. This first public
engine prerelease since \`.9\` consolidates the tested \`.10\` through \`.18\` revisions into a more configurable,
load-aware implementation with reliable recovery from SteamOS/Game Mode presentation disruptions.

### Highlights

- Expands opt-in Adaptive Frame Generation with a 2x/3x/4x quality ceiling and optional Smooth Cadence. Fixed 2x, 3x,
  and 4x remain available and unchanged.
- Protects real-frame throughput by ramping generation gradually, retaining proven levels, measuring suspected
  load-induced collapses, and backing off counterproductive probes.
- Keeps temporal history current during Gamescope fallback, restores validated Adaptive state after recovery, and can
  request one guarded swapchain rebuild when stale presentation state remains.
- Preserves gameplay cadence across menu/focus transitions and excludes impossible DX12/VKD3D fast-present bursts from
  Adaptive's feedback loop.
- Adds process-unique context IDs and focused diagnostics for presentation stalls, recovery, and Adaptive policy.

### Important limitations

- Adaptive Frame Generation is experimental and opt-in. This independent Vulkan-layer scheduler varies between zero
  and three generated frames per real frame toward the configured average target. It cannot reduce a native framerate
  already above the target, exceed the selected 4x maximum, guarantee an unreachable target, or provide the Windows
  Queue Target modes.
- The 0x multiplier from lsfg-vk 1.x is not present in upstream v2. Use \`DISABLE_LSFGVK=1\` or remove the launch
  wrapper and restart the game when frame generation must be disabled.
- Higher interpolation ratios and lower real-frame rates can increase ghosting and input latency. Smooth Cadence can
  improve motion consistency but may lower real-frame cadence and responsiveness, so it defaults to disabled.
- Lossless Scaling and \`Lossless.dll\` must already be installed through Steam; neither release archive includes or
  modifies it.
- \`LSFGVK_PRESENT_RECOVERY_RECREATE=1\` is an opt-in Adaptive recovery path for direct engine users. A swapchain
  rebuild can briefly pause or flicker, and some games may mishandle it. The Decky experimental wrapper enables the
  tested timeout and guarded rebuild automatically.
- Flatpak extensions for 23.08, 24.08, and 25.08 are packaged separately in
  \`$(basename "$flatpak_archive")\` under a dedicated experimental ID that can coexist with the public Flathub layer.

### Included files

- Vulkan implicit layer: \`liblsfg-vk-layer.so\`
- CLI and Qt configuration UI
- Vulkan manifest and XDG desktop files

### Adaptive configuration

Enable the new mode through the Qt UI or a profile:

\`\`\`toml
adaptive = true
target_fps = 120
adaptive_max_multiplier = 3
adaptive_stable_cadence = false
\`\`\`

Restart the game after switching between Fixed and Adaptive modes so the swapchain has the intended generated-frame
capacity. The target, maximum multiplier, Smooth Cadence, flow scale, and performance mode can then be hot-reloaded.

### Improvements

- Adds a configurable 2x/3x/4x Adaptive ceiling. When the target is unreachable at the selected quality limit, output
  remains below target instead of silently using a higher interpolation ratio.
- Stabilizes on real frames, ramps generated workload one level at a time, and accepts a step only when it improves
  useful output without an unsafe real-rate collapse. A bounded bridge probe handles misleading Gamescope divisors.
- Separates interrupted probes from genuine failures. Interrupted work rearms after two stable seconds; rejected probes
  use progressive cooldowns and can retry early after a sustained 15% base-rate improvement.
- Treats 95% of the target as satisfied and requires a smaller remaining deficit to persist for one second before
  testing more expensive work.
- Adds optional Smooth Cadence for suitable fractional targets, with strict target scheduling and all load protections
  retained when the option is disabled.
- Monitors a newly accepted higher level after its initial probe. If full load later collapses real-frame throughput,
  Adaptive measures one second without generated work and restores the lower proven level only when cadence recovers.
- Preserves the validated generation level and gameplay baseline across recovery and hard menu/focus stalls. Sustained
  gameplay slowdowns instead rebase after normal one-second stabilization.
- Warms all three shared temporal-history slots before Adaptive first generates output and keeps shared history current
  while generated images are unavailable.

### Fixed

- Prevents the existing Gamescope inference-bypass fallback from advancing temporal-history counters without refreshing
  the shared feature slots used when generation resumes.
- Prevents startup and post-recovery ghosting caused by partially initialized or stale shared temporal-history slots.
- Prevents repeated menu transitions from accumulating stale Adaptive credit, validating a multiplier while generation
  is bypassed, or immediately rebuilding the same harmful load after swapchain recovery.
- Prevents a recovered context from entering a swapchain recreate-and-retry loop through a five-second cross-context
  cooldown and a real-frame stabilization period.
- Prevents strict Adaptive from remaining trapped at an accepted higher multiplier that later performs worse than the
  previous proven level. Suspected delayed collapses are confirmed with one second of real-only measurement.
- Starts delayed-load rescue below 80% base-rate retention while preserving persistence checks, so sustained
  degradation is handled earlier without reacting to an isolated hitch.
- Prevents Steam-menu interruptions from being counted as genuine probe failures or imposing unnecessary long
  cooldowns.
- Excludes implausibly fast DX12/VKD3D presentation bursts from cadence smoothing and pauses policy evaluation until
  ordinary cadence returns.

### Optional diagnostics

Presentation diagnostics remain disabled by default. When enabled, slow Vulkan operations, fallback/recovery state,
Adaptive ramp decisions, fast-cadence bursts, and swapchain lifecycle events include a process-unique
\`context=<ID>\` so concurrent or replacement contexts can be separated.

With the isolated Decky LSFG-VK Experimental plugin, enable diagnostics with this Steam launch option:

\`\`\`bash
LSFGVK_PRESENT_DIAGNOSTICS=1 LSFGVK_PRESENT_DIAGNOSTICS_THRESHOLD_MS=25 ~/.local/bin/lsfg-vk-experimental %command%
\`\`\`

After reproducing the problem, extract the latest diagnostic entries with:

\`\`\`bash
grep -aF "lsfg-vk: present diagnostics:" ~/.steam/steam/logs/console-linux.txt | tail -n 800
\`\`\`

See the
[presentation-stall troubleshooting guide](https://github.com/eugeniosegala/lsfg-vk-experimental/blob/develop/docs/Troubleshooting.md#diagnosing-presentation-stalls)
for recovery variables, event meanings, and focused filters. Disable diagnostics after collecting the trace.

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
