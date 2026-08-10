# Upstream tracking ledger

This repository follows [`PancakeTAS/lsfg-vk`](https://github.com/PancakeTAS/lsfg-vk) through the local `origin`
remote. It is intentionally not a blind mirror: this ledger records every commit carried on top of the reviewed upstream
baseline so a future update can distinguish upstream work from experimental-fork work.

## Reviewed baseline

| Item                        | Value                                                                                                               |
|-----------------------------|---------------------------------------------------------------------------------------------------------------------|
| Upstream repository         | [`PancakeTAS/lsfg-vk`](https://github.com/PancakeTAS/lsfg-vk)                                                       |
| Upstream branch             | `develop`                                                                                                           |
| Baseline commit             | [`8b0da266`](https://github.com/PancakeTAS/lsfg-vk/commit/8b0da2661c6f3473a7fccc8ba643880050e71642)                 |
| Experimental branch         | `develop` → `experimental/develop`                                                                                  |
| Current experimental branch | [`develop`](https://github.com/eugeniosegala/lsfg-vk-experimental/commits/develop)                                   |
| Reviewed on                 | 2026-08-09                                                                                                          |

## Changes carried on top of upstream

| Experimental commit                                                                                                | Source or reason                                                                                                                                                              | What it changes                                                                                               |
|--------------------------------------------------------------------------------------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|---------------------------------------------------------------------------------------------------------------|
| [`e250ca5`](https://github.com/eugeniosegala/lsfg-vk-experimental/commit/e250ca577c9a80776ffe0111b6ea54eafc8a67e2) | [Upstream PR #544](https://github.com/PancakeTAS/lsfg-vk/pull/544), commit [`545f37a`](https://github.com/PancakeTAS/lsfg-vk/commit/545f37a58ca3ad37213ae84cc0a4fa4078def90a) | Clamps mip extents to at least 1, preventing a zero-sized mip image during transient small-swapchain startup. |
| [`5a49614`](https://github.com/eugeniosegala/lsfg-vk-experimental/commit/5a49614ef6e49ff4ed987513edc94e9e33689006) | [Upstream PR #544](https://github.com/PancakeTAS/lsfg-vk/pull/544), commit [`f4f7444`](https://github.com/PancakeTAS/lsfg-vk/commit/f4f744429ac6362098e4ebd2d923e87e59d8171e) | Converts a null `vkAllocateMemory` handle into a normal lsfg-vk error path.                                   |
| [`f766cba`](https://github.com/eugeniosegala/lsfg-vk-experimental/commit/f766cba2e0c59d560d505f5f403c3c0673143502) | Experimental-fork maintenance                                                                                                                                                 | Adds the fork identity and points documentation to this repository.                                           |
| [`bb61381`](https://github.com/eugeniosegala/lsfg-vk-experimental/commit/bb61381e35dad7534cf3e154047287260a88bf8c) | Experimental-fork maintenance                                                                                                                                                 | Adds script-driven local release packaging.                                                                   |
| [`6b41dca`](https://github.com/eugeniosegala/lsfg-vk-experimental/commit/6b41dca10d819d94dad993b1f72b5af4d97466f5) | Experimental-fork maintenance                                                                                                                                                 | Packages Linux releases through the local Docker workflow.                                                    |
| [`82e0d49`](https://github.com/eugeniosegala/lsfg-vk-experimental/commit/82e0d49976db8bce5e472fa154526e971529e091) | Experimental-fork maintenance                                                                                                                                                 | Documents the current configuration limits in the release notes.                                              |

The two PR #544 changes were checked on 2026-08-05 against their original commits: their changed-file patches are
identical. The commit IDs differ because the changes were carried into this repository rather than merged from the
upstream branch. The PR was open when checked; when it lands upstream, compare the merged patch before deciding whether
these two carried commits can be retired.

## Experimental packaging release: `v2.0.0-dev28-experimental.2`

This release does not change the lsfg-vk rendering code. It packages the existing reviewed dev28 engine for the
experimental Decky integration, including support for sandboxed Flatpak applications such as Heroic.

| Item                        | Value                                                                                         |
|-----------------------------|-----------------------------------------------------------------------------------------------|
| Host engine lineage         | `2.0.0-dev28-experimental.1` host payload, rebuilt as part of this release                    |
| New artifact                | `lsfg-vk-2.0.0-dev28-experimental.2-flatpaks.tar.xz`                                          |
| Supported Flatpak runtimes  | Freedesktop `23.08`, `24.08`, and `25.08`                                                     |
| Experimental extension ID   | `org.freedesktop.Platform.VulkanLayer.lsfgvkexperimental`                                     |
| Experimental install prefix | `/usr/lib/extensions/vulkan/lsfgvkexperimental`                                               |
| Coexistence rationale       | The unique ID and prefix keep this layer distinct from the public `lsfgvk` Flathub extension. |

The separate extension ID is intentional. It allows a user to keep the public and experimental layers installed, while
the Decky plugin selects the experimental one only for Flatpak applications the user explicitly enables. It does not
modify or replace the public Flathub extension.

## Flatpak packaging hotfix: `v2.0.0-dev28-experimental.3`

This release fixes the experimental Flatpak extensions only; it does not change the host rendering code or the
reviewed dev28 lineage. The previous extensions installed `liblsfg-vk-layer.so` in `lib64`, while their Vulkan
manifests incorrectly referenced `lib`. Vulkan could therefore discover the manifest but could not load the layer in a
sandboxed application such as Heroic.

All supported Flatpak manifests now reference:

```text
/usr/lib/extensions/vulkan/lsfgvkexperimental/lib64/liblsfg-vk-layer.so
```

The Flatpak packaging script now validates both the installed library and the manifest path before creating a bundle,
so this mismatch fails the release build instead of reaching users.

## Presentation diagnostic release: `v2.0.0-dev28-experimental.4`

This release fixes an undefined access in the Vulkan submission helper when no timeline semaphore is present. It also
adds opt-in timing diagnostics around frame scheduling, render-fence waits, generated-image acquisition, GPU copy
submission, and generated/original presentation calls.

The diagnostics are intended to locate the SteamOS/Game Mode overlay transition stall before changing synchronization
behaviour. They are disabled by default, and this release does not yet claim to resolve that presentation issue. See
[`docs/Troubleshooting.md`](docs/Troubleshooting.md#diagnosing-presentation-stalls) for the diagnostic launch option and
log extraction command.

## Gamescope presentation-recovery test release: `v2.0.0-dev28-experimental.5`

Diagnostics collected on SteamOS showed that the Steam-menu slowdown is dominated by
`vkAcquireNextImageKHR` waiting for an extra swapchain image used for a generated frame. Individual waits reached
approximately 74 ms, while the total presentation duration closely tracked the acquisition duration.

This release adds an opt-in recovery path controlled by `LSFGVK_PRESENT_ACQUIRE_TIMEOUT_MS`. When acquisition exceeds
the configured timeout, the layer waits for the backend's already-scheduled work, skips the remaining generated frames
for that presentation, presents the original game frame, and resumes normal generation on the next source frame. The
frontend and backend timeline values remain aligned, and the normal render fence is signalled before command-buffer
reuse.

The recovery remains opt-in for this test release because a fixed timeout must be validated across refresh rates,
frame multipliers, pacing modes, and games before it can safely become the default. See
[`docs/Troubleshooting.md`](docs/Troubleshooting.md#diagnosing-presentation-stalls) for the test launch option and log
extraction command.

## Gamescope non-blocking recovery test release: `v2.0.0-dev28-experimental.6`

Testing `.5` confirmed that its synchronization fallback completed successfully, but also revealed that repeated
acquisition attempts each waited for the full configured timeout. During one captured slowdown, every frame spent
approximately 25–27 ms acquiring an image before falling back, producing total presentation times of roughly 27–35
ms.

After the first timeout, `.6` therefore changes subsequent acquisition attempts to non-blocking probes. The layer
continues presenting original game frames while Gamescope has no spare image and resumes generated frames immediately
when an image becomes available. Diagnostic fallback entries identify `initial-timeout` and `nonblocking-retry` modes,
and a `resume-generated-frames` entry identifies successful recovery.

## Gamescope inference-bypass recovery test release: `v2.0.0-dev28-experimental.7`

SteamOS testing of `.6` showed that the non-blocking recovery reliably avoided persistent stalls, but the engine still
scheduled frame-generation work on every retry before discovering that Gamescope had no image available. The generated
output was then discarded, consuming GPU time while the Steam menu and compositor were already under pressure.

During backoff, `.7` probes the first generated-image slot before scheduling the model. If it remains unavailable, the
layer still copies and presents the current real frame so the game and compositor continue progressing, but it advances
the shared timeline and backend frame indices without dispatching unused inference work. Source-image parity remains
aligned, so recovery uses the two latest real frames as soon as Gamescope returns an image. The initial timeout path is
unchanged and still waits for any work that was already scheduled.

Diagnostics now distinguish `backend_work=scheduled` on the initial timeout from `backend_work=bypassed` during
backoff. Repeated expected `VK_NOT_READY` results are aggregated instead of logged every frame; the recovery entry
reports the total as `bypassed_frames`.

## Gamescope bounded-reacquisition test release: `v2.0.0-dev28-experimental.8`

Testing `.7` showed that its zero-timeout backoff kept the Steam menu responsive and avoided wasted inference work, but
the probe could repeatedly run at a point where the application had already acquired the only currently available
swapchain image. Captured recoveries consequently took 225 and 529 real frames (approximately four and nine seconds at
60 fps), despite each probe itself returning immediately.

After one second of fallback, `.8` now makes one bounded reacquisition attempt per second using the configured
`LSFGVK_PRESENT_ACQUIRE_TIMEOUT_MS` value. All intervening attempts remain non-blocking and continue bypassing model
work. This gives Gamescope a short window to release an image without returning a synthetic out-of-date result or
forcing the game to recreate its swapchain. Diagnostics identify these periodic attempts with
`acquire_mode=bounded-retry`.

## Adaptive Frame Generation test build: `v2.0.0-dev28-experimental.9`

This release adds an opt-in Adaptive scheduler on top of the reviewed dev28 engine. Profiles can set
`adaptive = true` and a `target_fps`; Fixed mode remains the default and retains its existing multiplier path.

The scheduler measures the real-frame interval, keeps a fractional output budget, and selects between zero and three
generated frames for each real frame. Before dispatch, it updates the model's normalized interpolation timestamps so
the chosen outputs remain evenly positioned between the two source frames. It suspends interpolation when the measured
base rate drops below 10 FPS and discards impossible backlog above the 4x safety ceiling.

This is not a copy of Lossless Scaling's Windows capture engine. An inline Vulkan layer cannot present fewer real
frames than the application submits, so it cannot reduce a base framerate already above the requested target. It also
cannot reach targets above four times the current base rate. These limitations are exposed in both configuration UIs
and must remain in release notes while the feature is experimental.

## Temporal-history recovery test build: `v2.0.0-dev28-experimental.10`

SteamOS Adaptive testing at a 30 FPS base and 120 FPS target confirmed that the scheduler remained at three generated
frames before and after a Gamescope overlay transition. The increasing ghosting was therefore not caused by multiplier
escalation. The trace instead showed a partial generated-image timeout followed by four real frames of backend bypass.

The `.7` recovery kept source-image parity and timeline counters aligned, but it advanced the model's real-frame index
without running the shared pre-pass. The model's multi-frame temporal feature slots consequently contained older data
when generation resumed. This is especially visible at Adaptive 4x because every affected interval displays three
generated outputs.

This build replaces counter-only bypass with a history-only pre-pass. It updates mipmaps and the shared alpha/beta
temporal features for every real frame while continuing to skip the expensive per-output gamma/delta/generation passes
until Gamescope releases an image. Adaptive timing credit is also reset when acquisition enters or leaves backoff so a
compositor discontinuity is not carried into later scheduling decisions. Diagnostics report
`backend_work=history-only` for this path.

## Adaptive history warm-up test build: `v2.0.0-dev28-experimental.11`

SteamOS testing of `.10` showed that continuous history maintenance improved recovery but did not make the temporal
handoff reliable. One trace recorded repeated single-image recoveries followed by immediate new acquisition failures:
15 bypassed frames, then 16, 31, and 9 before Gamescope remained stable. Adaptive restarted up to three generated
outputs after each single successful probe. Testers also observed occasional high ghosting immediately after game
startup that cleared after reinitializing Adaptive mode.

The `.11` candidate adds a three-real-frame history warm-up both when an Adaptive context starts and after a Gamescope
recovery. Three frames match the deepest shared temporal ring. During recovery, the image owned by the successful
probe is filled with the real game frame and presented so no acquired swapchain image is leaked, while generated
outputs remain disabled. The following history frames continue presenting real output; Adaptive timing remains reset
throughout the warm-up. If acquisition fails again later, the next successful recovery starts a fresh warm-up.

This deliberately trades approximately three base-frame intervals for a clean temporal handoff. At a 30 FPS base it
is roughly 100 ms. Fixed mode retains its immediate recovery path. Diagnostics distinguish `generated-image-recovered`
from `history-warmup` and identify whether warm-up was caused by `startup` or `recovery`.

## Adaptive quality-limit test build: `v2.0.0-dev28-experimental.12`

SteamOS comparison testing showed that Adaptive output matched Fixed 2x image quality when both produced approximately
100 FPS from the same real-frame rate. Raising only the Adaptive target to 120 FPS increased ghosting because the
scheduler had to mix 2x and 3x intervals. This was expected target-first behaviour, not additional temporal-history
corruption: every real frame remained present, but a larger fraction of displayed frames was generated.

The `.12` candidate adds `adaptive_max_multiplier = 2|3|4`, defaulting to `3`. The target remains the requested output
rate, while this independent ceiling defines the highest acceptable interpolation ratio. If the real-frame rate falls
far enough that the target would require a higher ratio, Adaptive deliberately undershoots the target rather than
adding more artifact-prone generated frames. The standalone UI and `LSFGVK_ADAPTIVE_MAX_MULTIPLIER` environment path
expose the same setting. Fixed mode remains unchanged.

## Adaptive swapchain-recreation recovery test build: `v2.0.0-dev28-experimental.13`

SteamOS testing showed that the `.11`/`.12` history recovery substantially improved Game Mode transitions, but repeated
Steam-menu cycles could still accumulate input latency. Capping Adaptive at 2x controlled interpolation artifacts but
did not clear that latency, and in some runs toggling Adaptive off and on could not recover without restarting the
game. This indicates stale game-owned swapchain or Gamescope presentation state rather than only stale model history.

The `.13` candidate adds the opt-in `LSFGVK_PRESENT_RECOVERY_RECREATE=1` policy. It activates only after a configured
generated-image acquire timeout has entered fallback and a later probe has successfully reacquired an image. The layer
first fills and presents that acquired image with the real game frame, presents the application's original image, and
then returns `VK_ERROR_OUT_OF_DATE_KHR`. This standard Vulkan result asks the application to destroy and recreate its
swapchain, which also creates a fresh LSFG context and runs the normal Adaptive startup history warm-up.

The request is delayed until image availability has recovered, avoiding recreation loops while the Steam overlay still
owns the presentation images. Once requested, the old LSFG swapchain continues returning out-of-date until the game
replaces it. Fixed mode is unchanged. The standalone engine remains unchanged unless both the acquire timeout and the
new recovery variable are explicitly enabled; the experimental Decky wrapper opts in for local validation.

Some games can mishandle a synthetic out-of-date result, so this remains a guarded test rather than a general default.
Set `LSFGVK_PRESENT_RECOVERY_RECREATE=0` to retain the three-frame Adaptive recovery warm-up without forcing the
game-owned swapchain to be recreated.

## Adaptive stabilization test build: `v2.0.0-dev28-experimental.14`

SteamOS testing of `.13` confirmed that swapchain recreation could clear accumulated presentation latency, but a
recreated context could immediately request the maximum Adaptive load while the game and Gamescope were still
settling. On a low base rate, this could collapse real-frame throughput, increase interpolation distance and trigger
another acquire timeout. Repeated Steam-menu transitions could therefore enter a recreate-and-retry loop.

The `.14` candidate keeps Fixed mode unchanged and adds an Adaptive-only recovery controller:

- Adaptive presents real frames for one second after startup, recovery, or a sustained cadence discontinuity.
- Generated-frame load ramps from 0 to 1 to 2 or 3 intermediates instead of jumping directly to the configured limit.
- Each step is evaluated against real-frame throughput. A step that reduces useful output or collapses base FPS for
  only a marginal output gain is rolled back and retried after five seconds.
- Swapchain recreation requests share a five-second cooldown across replacement contexts, preventing a recovered
  context from immediately starting another recreation loop.
- Opt-in diagnostics now report stabilization, ramp decisions, load shedding, cooldown suppression, and swapchain
  context creation/destruction.

This policy deliberately prioritizes stable base-frame cadence and temporal quality over reaching the target at any
cost. Adaptive may remain below the requested target when a higher multiplier would be counterproductive.

## Adaptive bounded-bridge test build: `v2.0.0-dev28-experimental.15`

SteamOS traces from `.14` showed a remaining recovery failure that looked like a permanent detach. At a real base rate
near 60 FPS, testing one intermediate frame could move Gamescope to a roughly 30 FPS cadence. The scheduler then saw
little estimated output gain, returned to zero generated frames, waited five seconds, and repeated the same 0-to-1
probe. It never tested whether two intermediates could escape that compositor cadence divisor and produce a useful
output increase.

The `.15` candidate keeps Fixed mode unchanged and makes Adaptive probing bounded and stateful:

- A rejected first step may make one one-second bridge test at two generated frames, but only when the first step kept
  at least 85% of the original estimated output, the original output remained well below target, and the configured
  maximum permits the bridge.
- The bridge is accepted only when it improves estimated output by at least 15%, retains at least 40% of the original
  real-frame rate, and stays above the 10 FPS safety floor. Otherwise the scheduler returns to real frames.
- A failed bridge, rejected first step, or cadence interruption during a probe schedules a 15-second cooldown and then
  requires two continuous seconds of stable cadence before another attempt.
- New diagnostics distinguish bridge attempts, accepted/rejected bridges, interrupted probes, and rearm scheduling.
- Load-shedding decisions do not request swapchain recreation. Existing acquire-timeout recovery remains separately
  guarded by `LSFGVK_PRESENT_RECOVERY_RECREATE`.

The bridge can briefly increase interpolation load for its one-second evaluation window. It is intentionally attempted
only once per recovery interval and is rolled back when it does not demonstrate a meaningful benefit.

## Adaptive retained-level test build: `v2.0.0-dev28-experimental.16`

The `.16` candidate carries the last validated generated-frame level through Gamescope recovery. The replacement
context still performs its real-frame stabilization and temporal-history warm-up, then resumes that proven level
instead of always rebuilding Adaptive load from zero. Higher-level probes remain delayed so recovery does not
immediately repeat the load that contributed to a disruption.

## Adaptive cadence and retry test build: `v2.0.0-dev28-experimental.17`

The `.17` candidate adds bounded constant-cadence validation for suitable fractional targets, retains the validated
level across a replacement swapchain, and progressively backs off repeatedly rejected higher-level probes. Adaptive
policy evaluation is frozen while generated output is bypassed, preventing the cheaper real-frame-only fallback from
falsely validating a multiplier.

Profiles can set `adaptive_stable_cadence = false` to disable only constant-cadence validation. The default remains
`true` for compatibility with the earlier `.17` candidate. Strict target scheduling is used when it is disabled, while
Adaptive recovery, load shedding, multiplier limits, and retry backoff remain active. The standalone configuration UI
and `LSFGVK_ADAPTIVE_STABLE_CADENCE=0` environment path expose the same option. Fixed mode remains unchanged.

## Update procedure

1. Fetch the upstream branch and inspect what changed since the reviewed baseline:

   ```bash
   git fetch origin develop --tags
   git log --oneline 8b0da2661c6f3473a7fccc8ba643880050e71642..origin/develop
   ```

2. Compare the upstream and experimental change ranges:

   ```bash
   git range-diff 8b0da2661c6f3473a7fccc8ba643880050e71642..origin/develop \
     8b0da2661c6f3473a7fccc8ba643880050e71642..develop
   ```

3. Decide which entries in the table still need to be carried. In particular, verify whether the merged upstream change
   for PR #544 is equivalent before dropping the mip-extent and null-memory commits.
4. Build and test the engine, then update the baseline and this ledger in the same commit as any rebase, merge, or
   additional carried patch.
