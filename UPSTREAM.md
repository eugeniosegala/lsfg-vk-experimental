# Upstream tracking ledger

This repository follows [`PancakeTAS/lsfg-vk`](https://github.com/PancakeTAS/lsfg-vk) through the local `origin`
remote. It is intentionally not a blind mirror: this ledger records every commit carried on top of the reviewed upstream
baseline so a future update can distinguish upstream work from experimental-fork work.

## Current status

| Item                       | Value                                                                                  |
|----------------------------|----------------------------------------------------------------------------------------|
| Release version            | `2.0.0-dev28-experimental.21`                                                          |
| Release basis              | Known-good `aeae16f` runtime after local Deck hardware validation                      |
| Pre-publication validation | Deterministic suite, 120-case matrix, native/Flatpak packaging, and hardware testing   |
| Included change range      | `.21`: scheduler extraction, Smooth Cadence ceiling, and UI/docs corrections           |
| Retired local experiment   | `ab4f790` hot-path changes removed after intermittent generation flinches              |
| Fixed-mode impact          | Fixed 2x, 3x, and 4x scheduling remains on its existing path                          |
| Ledger reconciled          | 2026-08-12                                                                             |

The sections below are chronological. Versions through `.9` document the original published prereleases; `.10` through
`.17` record the successive local test builds consolidated into the `.18` release; `.19` records the follow-up 2x
gameplay-hitch refinement; `.20` adds the live frame-generation switch; and `.21` extracts and validates the Adaptive
policy state machine. The detailed history is intentionally retained here so the public README and release notes can
remain concise.

## Reviewed baseline

| Item                        | Value                                                                                                               |
|-----------------------------|---------------------------------------------------------------------------------------------------------------------|
| Upstream repository         | [`PancakeTAS/lsfg-vk`](https://github.com/PancakeTAS/lsfg-vk)                                                       |
| Upstream branch             | `develop`                                                                                                           |
| Baseline commit             | [`8b0da266`](https://github.com/PancakeTAS/lsfg-vk/commit/8b0da2661c6f3473a7fccc8ba643880050e71642)                 |
| Experimental branch         | `develop` → `experimental/develop`                                                                                  |
| Current experimental branch | [`develop`](https://github.com/eugeniosegala/lsfg-vk-experimental/commits/develop)                                   |
| Reviewed on                 | 2026-08-11                                                                                                          |

The upstream `develop` reference was fetched again on 2026-08-11 and still pointed to the baseline above.

## Initial carried changes and repository setup

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

The chronological build history below records the later engine, configuration, diagnostic, and packaging work. Git
history remains authoritative for exact patches; this ledger records why each change exists and how it was validated.

## Experimental build history

### Initial experimental release: `v2.0.0-dev28-experimental.1`

The first fork release established the reviewed upstream dev28 baseline, carried the two PR #544 safety fixes listed
above, branded the experimental distribution, and replaced GitHub Actions with local Linux/Docker packaging scripts.
It did not add Adaptive scheduling or Gamescope recovery; those changes begin in the later entries below.

### Experimental packaging release: `v2.0.0-dev28-experimental.2`

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

### Flatpak packaging hotfix: `v2.0.0-dev28-experimental.3`

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

### Presentation diagnostic release: `v2.0.0-dev28-experimental.4`

This release fixes an undefined access in the Vulkan submission helper when no timeline semaphore is present. It also
adds opt-in timing diagnostics around frame scheduling, render-fence waits, generated-image acquisition, GPU copy
submission, and generated/original presentation calls.

The diagnostics are intended to locate the SteamOS/Game Mode overlay transition stall before changing synchronization
behaviour. They are disabled by default, and this release does not yet claim to resolve that presentation issue. See
[`docs/Troubleshooting.md`](docs/Troubleshooting.md#diagnosing-presentation-stalls) for the diagnostic launch option and
log extraction command.

### Gamescope presentation-recovery test release: `v2.0.0-dev28-experimental.5`

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

### Gamescope non-blocking recovery test release: `v2.0.0-dev28-experimental.6`

Testing `.5` confirmed that its synchronization fallback completed successfully, but also revealed that repeated
acquisition attempts each waited for the full configured timeout. During one captured slowdown, every frame spent
approximately 25–27 ms acquiring an image before falling back, producing total presentation times of roughly 27–35
ms.

After the first timeout, `.6` therefore changes subsequent acquisition attempts to non-blocking probes. The layer
continues presenting original game frames while Gamescope has no spare image and resumes generated frames immediately
when an image becomes available. Diagnostic fallback entries identify `initial-timeout` and `nonblocking-retry` modes,
and a `resume-generated-frames` entry identifies successful recovery.

### Gamescope inference-bypass recovery test release: `v2.0.0-dev28-experimental.7`

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

### Gamescope bounded-reacquisition test release: `v2.0.0-dev28-experimental.8`

Testing `.7` showed that its zero-timeout backoff kept the Steam menu responsive and avoided wasted inference work, but
the probe could repeatedly run at a point where the application had already acquired the only currently available
swapchain image. Captured recoveries consequently took 225 and 529 real frames (approximately four and nine seconds at
60 fps), despite each probe itself returning immediately.

After one second of fallback, `.8` now makes one bounded reacquisition attempt per second using the configured
`LSFGVK_PRESENT_ACQUIRE_TIMEOUT_MS` value. All intervening attempts remain non-blocking and continue bypassing model
work. This gives Gamescope a short window to release an image without returning a synthetic out-of-date result or
forcing the game to recreate its swapchain. Diagnostics identify these periodic attempts with
`acquire_mode=bounded-retry`.

### Adaptive Frame Generation release: `v2.0.0-dev28-experimental.9`

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

### Temporal-history recovery test build: `v2.0.0-dev28-experimental.10`

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

### Adaptive history warm-up test build: `v2.0.0-dev28-experimental.11`

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

### Adaptive quality-limit test build: `v2.0.0-dev28-experimental.12`

SteamOS comparison testing showed that Adaptive output matched Fixed 2x image quality when both produced approximately
100 FPS from the same real-frame rate. Raising only the Adaptive target to 120 FPS increased ghosting because the
scheduler had to mix 2x and 3x intervals. This was expected target-first behaviour, not additional temporal-history
corruption: every real frame remained present, but a larger fraction of displayed frames was generated.

The `.12` candidate adds `adaptive_max_multiplier = 2|3|4`, defaulting to `3`. The target remains the requested output
rate, while this independent ceiling defines the highest acceptable interpolation ratio. If the real-frame rate falls
far enough that the target would require a higher ratio, Adaptive deliberately undershoots the target rather than
adding more artifact-prone generated frames. The standalone UI and `LSFGVK_ADAPTIVE_MAX_MULTIPLIER` environment path
expose the same setting. Fixed mode remains unchanged.

### Adaptive swapchain-recreation recovery test build: `v2.0.0-dev28-experimental.13`

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

### Adaptive stabilization test build: `v2.0.0-dev28-experimental.14`

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

### Adaptive bounded-bridge test build: `v2.0.0-dev28-experimental.15`

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

### Adaptive retained-level test build: `v2.0.0-dev28-experimental.16`

The `.16` candidate carries the last validated generated-frame level through Gamescope recovery. The replacement
context still performs its real-frame stabilization and temporal-history warm-up, then resumes that proven level
instead of always rebuilding Adaptive load from zero. Higher-level probes remain delayed so recovery does not
immediately repeat the load that contributed to a disruption.

### Adaptive cadence and retry test build: `v2.0.0-dev28-experimental.17`

The `.17` candidate adds bounded constant-cadence validation for suitable fractional targets, retains the validated
level across a replacement swapchain, and progressively backs off repeatedly rejected higher-level probes. Adaptive
policy evaluation is frozen while generated output is bypassed, preventing the cheaper real-frame-only fallback from
falsely validating a multiplier.

Profiles can set `adaptive_stable_cadence = true` to enable constant-cadence validation. It now defaults to `false`
after Steam Deck testing confirmed the expected trade-off: constant cadence looks smoother but can lower the real-frame
presentation rate and feel less responsive. Strict target scheduling is used when it is disabled, while
Adaptive recovery, load shedding, multiplier limits, and retry backoff remain active. The standalone configuration UI
and `LSFGVK_ADAPTIVE_STABLE_CADENCE=1` environment path expose the same opt-in. Fixed mode remains unchanged.

Smooth Cadence now waits two seconds after a successful generation ramp and activates only when strict scheduling
already requests at least 95% of the matching integer output cadence. If a validated cadence later loses at least 22%
of its base rate and falls below 80% of the requested output for 500 ms, Adaptive performs one second of real-only
measurement. It then resumes fractional scheduling or probes one higher generated-frame level when
`adaptive_max_multiplier` permits it. Rescue attempts never exceed the configured maximum and have a 15-second
cooldown to prevent oscillation. If the measured real rate still cannot reach the target at the configured ceiling,
Adaptive keeps the best permitted level instead of repeatedly retrying.

SteamOS traces from the later `.17` candidate exposed a separate abrupt-transition path. Opening the Steam menu could
produce a raw cadence stall before the smoothed-cadence collapse detector ran. Stabilization then discarded the
healthy base-rate baseline, the first recovered generated image immediately requested swapchain recreation, and the
new context rebuilt its multiplier from transient 10–30 FPS samples. The trace contained repeated cadence stalls,
load-shed decisions, and recreations but no `adaptive-rescue-*` event.

The revised `.17` candidate therefore adds bounded discontinuity recovery for Adaptive scheduling, independently of
the Smooth Cadence toggle. Later Steam Deck testing showed that treating every sustained cadence drop as a hard
discontinuity could leave a demanding gameplay scene waiting five seconds for an old rate that was no longer reachable.
Only a hard cadence stall now retains that old baseline; a sustained gameplay drop stabilizes for one second and rebases
at the new rate:

- A hard cadence stall retains the last validated generation level and pre-transition smoothed base rate.
- A sustained cadence drop uses the ordinary one-second stabilization, then ramps against its new measured rate.
- The scheduler presents real frames until the measured base rate remains at least 90% of that baseline for one
  second, then restores the validated level without immediately probing higher.
- If the earlier cadence does not return within five seconds, the stale baseline is discarded and the normal guarded
  ramp restarts from zero using settled measurements.
- The first generated-image recovery in this window uses temporal-history warm-up without requesting a replacement
  swapchain. If acquisition stalls again, the existing guarded recreation path remains available and carries the
  discontinuity baseline and deadline into the replacement context.
- Diagnostics report `adaptive-discontinuity-recovery-start`, `phase=discontinuity-recovery`,
  `adaptive-discontinuity-soft-recovery`, and `adaptive-discontinuity-recovery-complete`.

Later traces showed that a ramp or bridge probe interrupted by a Steam-menu transition still inherited the same
15-second cooldown as a genuine throughput rejection. This produced an avoidable real-frame-only interval even though
the probe had never completed. The final `.17` policy separates those outcomes:

- An interrupted probe does not increment the failed-probe count and rearms after two stable seconds.
- A genuinely rejected first-step or bridge probe retains the 15-second cooldown.
- A rejected probe may rearm before the cooldown expires only when the real-only base rate improves by at least 15%
  over its pre-probe baseline and remains there for two seconds.
- Diagnostics expose `phase=rearm-cooldown`, the original rearm reason, remaining cooldown, baseline rate, and the
  decision that allowed rearming.

A Witcher 3 trace then exposed a strict-scheduler feedback trap outside Smooth Cadence: Fixed 2x retained roughly
60 real FPS and reached the display ceiling, while Adaptive could remain near 40 real FPS at its 3x ceiling long after
the higher level's one-second probe. The theoretical output estimate still equalled the target, so no later policy
reconsidered that accepted level. The final `.17` candidate now monitors the full load of a newly accepted strict level.
If real cadence remains below 70% of the previous level's baseline for one second and the higher level provides less
than a 15% estimated-output gain, it performs a one-second real-only measurement. If cadence recovers without
generated-frame work, it restores the previous proven level and delays another higher-load probe for 15 seconds. If
cadence does not recover, it retains the higher level because the game scene itself became more demanding. This
safeguard is independent of Smooth Cadence.
The ramp controller now also stops at the lowest validated level whose measured base-rate capacity reaches at least
98% of the requested target. If the base rate later falls below that threshold, the next level becomes eligible again.
For the reported Witcher 3 case, roughly 60 real FPS at 2x is therefore retained for a 120 FPS target instead of
unnecessarily probing 3x and risking the observed 40-FPS feedback state.

Fixed mode is unchanged. The bounded recovery can intentionally show real-frame output for up to five seconds instead
of applying interpolation against an unstable compositor cadence.

### Adaptive target stability and DX12 burst filtering release: `v2.0.0-dev28-experimental.18`

Follow-up SteamOS traces showed two remaining sources of Adaptive instability. First, a validated level close to the
requested target could still trigger a more expensive probe for a small or momentary deficit. Second, Witcher 3 on a
Heroic/DX12/VKD3D path produced transient presentation intervals corresponding to impossible 366–1,659 FPS estimates.
Those samples contaminated cadence smoothing and repeatedly forced real-only stabilization even though normal gameplay
remained near 59–63 FPS.

The `.18` release:

- Treats 95% of the target as satisfied instead of requiring 98%, reducing unnecessary higher-multiplier work near
  the display target.
- Requires a deficit below that threshold to remain present for one second before increasing the multiplier. A brief
  scene or compositor fluctuation therefore cannot start a new load probe by itself.
- Starts the delayed strict-load rescue when the accepted higher level retains less than 80% of the previous proven
  level's base rate, rather than waiting for a 70% collapse. The existing one-second persistence check and one-second
  real-only measurement remain in place so an isolated hitch is not mistaken for load-induced degradation.
- Preserves the separate 70% threshold used while evaluating an active ramp step; the earlier rescue threshold applies
  only after a higher level has already been accepted and later settles into a worse state.
- Adds a process-unique `context=<ID>` field to every presentation diagnostic, including swapchain lifecycle records,
  so concurrent windows and replacement contexts can be analysed independently.
- Rejects transient presentation intervals faster than both three times the proven base cadence and twice the Adaptive
  target before they enter cadence smoothing. This addresses a Witcher 3 DX12/VKD3D trace where a healthy 59–63 FPS
  baseline was replaced by impossible 366–1,659 FPS estimates, repeatedly forcing real-only stabilization.
- Pauses active ramp, Smooth Cadence, rescue, and stabilization evaluation windows while those invalid intervals are
  ignored. Pending target-deficit and collapse evidence is cleared, so a burst cannot validate an untested multiplier
  or trigger stale policy immediately afterward.
- Aggregates fast-burst diagnostics to at most one progress record per second plus a completion record, avoiding test
  distortion and excessive Steam log growth during a high-frequency burst.

#### Validation status

- The native 64-bit Linux archive compiled successfully and passed its packaging checks.
- Flatpak extensions for Freedesktop 23.08, 24.08, and 25.08 compiled successfully and passed their manifest/library
  layout checks.
- Witcher 3 DX12 testing no longer lost its established FPS after menu transitions. A subsequent trace recorded a
  569 FPS transient as `adaptive-fast-cadence-burst`, excluded it, and returned to normal cadence 17 ms later without
  changing the proven Adaptive level.
- Resident Evil testing captured genuine Gamescope acquisition stalls at the configured 50 ms bound. Real-frame
  fallback, reacquisition, guarded swapchain recreation, history warm-up, and Adaptive ramp all completed; no context
  remained detached or entered an endless recovery loop.

Local `.18` host and Flatpak archives were produced and runtime-tested before publication. The immutable artifacts,
checksums, tag, and GitHub prerelease are generated by `scripts/publish-package.sh` from the committed release source.

### Adaptive 2x gameplay-hitch recovery release: `v2.0.0-dev28-experimental.19`

Steam Deck testing of Call of Duty at a 90 FPS target with a 2x Adaptive ceiling identified a separate issue from
Gamescope image acquisition. The game remained in one healthy swapchain context and produced no generated-image
timeouts, but short heavy-scene hitches still entered the full Adaptive cadence-discontinuity path. That path presents
real frames while it waits for one second of healthy cadence, and can wait up to five seconds before a clean ramp. It
is appropriate for longer Steam-menu and focus interruptions, but it unnecessarily magnified an ordinary short
gameplay hitch when 2x was already the only available and validated generation level.

The `.19` release adds a narrow recovery path with these boundaries:

- It applies only when Adaptive is configured with a 2x ceiling and has already validated that 2x level.
- A hard cadence interval longer than 100 ms but no longer than 250 ms preserves the validated 2x policy, refreshes
  three real temporal-history frames, and resumes generation immediately afterwards.
- Longer interruptions retain the existing one-to-five-second discontinuity recovery, preserving the conservative
  Steam-menu and focus-transition handling.
- Generated-image acquisition fallback, the 50 ms Decky timeout, guarded swapchain recreation, Fixed mode, and
  Adaptive 3x/4x policies are unchanged.
- Diagnostics report `adaptive-gameplay-hitch-recovery` with the previous base rate, raw hitch duration, retained
  generation level, and history warm-up count.

The native Linux and Flatpak archives are rebuilt through the release scripts. Runtime testing remains game- and
hardware-dependent, so this remains an experimental prerelease rather than a guarantee of a locked target FPS.

### Live frame-generation toggle release: `v2.0.0-dev28-experimental.20`

[PacificSilent](https://github.com/PacificSilent) proposed restoring v1's live Off behaviour in
[PR #1](https://github.com/eugeniosegala/lsfg-vk-experimental/pull/1). The original contribution represented Off as
`multiplier = 1`. The experimental engine has separate Fixed and Adaptive controllers, so `.20` adapts the contribution
as an independent `frame_generation_enabled` switch instead of changing multiplier semantics.

- `frame_generation_enabled = false` directly presents the game's real swapchain images and performs no model
  scheduling, image copies, generated-image acquisition, or per-swapchain interpolation allocation.
- The selected Fixed multiplier or Adaptive target, maximum multiplier, and Smooth Cadence settings remain intact and
  are restored when the switch returns to `true`.
- The Vulkan layer and shared backend instance remain loaded. This is intentional: the watched configuration can
  recreate a fresh per-swapchain interpolation context and resume generation without restarting the game.
- The game-owned swapchain retains the capacity selected when it was created. This costs the same swapchain capacity
  as the selected enabled mode while live generation is off, but avoids a forced game restart or unsafe swapchain
  rebuild during re-enable.
- Off/on transitions clear cross-context Adaptive recovery state before the fresh context is created, preventing a
  stale recreation request, cooldown, or cadence baseline from leaking across the explicit user action.
- Fixed and Adaptive enabled paths retain their existing generated-frame capacity, model calls, recovery policy, and
  presentation sequence. Their only steady-state addition is the live-enabled boolean branch at the start of present.

The original feature commit remains authored by Jonathan Gallegos/PacificSilent. Compatibility, documentation, and UI
integration are layered separately so the contribution remains visible in the merged history.

### Deterministic Adaptive scheduler release: `v2.0.0-dev28-experimental.21`

The `.21` runtime is based on the locally validated `aeae16f` checkpoint. It preserves the `.20` Vulkan and recovery
paths while making Adaptive policy independently testable.

- `93323a4` extracts Adaptive policy into a clock-driven state machine with deterministic tests, a 120-case policy
  matrix, and a same-host scheduler microbenchmark.
- `aeae16f` prevents Smooth Cadence restoration and rescue from retaining a generated-frame level above the configured
  maximum.
- The native UI's Active In dialog no longer invokes profile creation through an unrelated confirmation callback, and
  a missing profile now reports Smooth Cadence's real default of disabled.
- Configuration documentation now distinguishes private interpolation-context hot reloads from game-owned swapchain
  capacity changes, especially after switching mode or increasing a Fixed multiplier.

The unpushed `ab4f790` hot-path experiment is deliberately excluded. It combined persistent coherent-buffer mappings,
inline submit-time semaphore storage, and global compute barriers; local hardware testing reported intermittent
generation flinches that were absent from `aeae16f`. Because those changes were bundled, the report does not prove which
one caused the regression. Any old local Decky package pinned to `ab4f790` is retired and must not be published. Future
performance work must be isolated and compared against `.21`; see `docs/Remaining-Improvements.md`.

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
