# Remaining improvements

This is the short working backlog following the `.21` release based on the known-good `aeae16f` runtime checkpoint. It
is intentionally narrower than the historical ledger in `UPSTREAM.md`.

## Retired hot-path experiment

The local `ab4f790` experiment combined three optimizations: persistent coherent-buffer mappings, stack-backed semaphore
arrays for normal submissions, and one global compute memory barrier before every dispatch. It was never pushed, tagged,
or published as an engine release. Hardware testing reported intermittent flinches in which generated output briefly
returned to the real-frame cadence; the same test setup considered `aeae16f` good. The branch was therefore moved back
to `aeae16f`.

The experiment did not alter shaders, dispatch counts, interpolation timestamps, or requested generated-frame counts, so
it had no intended image-quality trade-off. Nevertheless, synchronization changes can affect cache visibility and GPU
completion time. A broad barrier can also increase latency enough to expose an existing generated-image timeout or
Adaptive real-only recovery path. Visible quality changes, new hitches, output drops, or recovery events are all
regressions even when the average FPS improves.

Current risk ranking from the static audit and hardware report:

- **Global compute barrier:** highest risk. It is correctness-conservative, but it applies shader read/write ordering to
  every memory object before every dispatch and may cause wider cache work or serialization on a particular driver.
- **Persistent coherent-buffer mapping:** lower risk. Existing fences appear to protect host writes from prior GPU
  reads, but it must be tested independently across AMD, Intel, and NVIDIA drivers.
- **Stack-backed semaphore arrays:** lowest apparent risk. Counts, ordering, binary values, and timeline values were
  preserved, but this must still be isolated rather than assumed safe from review alone.

Do not restore the combined patch. Old local Decky packages with `perf1.ab4f790` in their version are retired test
artifacts and are not publication candidates.

## Performance work after profiling

- Add lightweight CPU timers around command recording and queue submission, plus GPU timestamp queries around the
  pre-pass and generated-output passes. Keep diagnostics opt-in and aggregate results to avoid creating new hitches.
- Establish whether the workload is GPU-compute, memory-bandwidth, driver, compositor, or CPU limited before changing
  synchronization or allocation behavior. There is no credible general ten-percent gain without removing meaningful
  work; smaller frametime-consistency gains are more realistic.
- Test one optimization per candidate build. Begin with the least invasive submit-array change, then persistent mapping.
  Reconsider barrier work only with per-resource dependency data and validation-layer/RenderDoc review.
- Prefer precise resource dependencies or a dependency graph over a global barrier issued before every dispatch. Never
  remove or broaden synchronization solely because the resulting command stream is shorter.
- Do not claim reduced ghosting from CPU or synchronization work. Ghosting requires matched captures at the same real
  cadence, generated ratio, model, flow scale, and camera motion.

## A/B measurement before publication

- Compare every future single-change candidate with the known-good `aeae16f` package in the same repeatable scene and
  power profile. Keep a rollback package installed and available.
- Record generation Off, Fixed 2x/3x/4x, and Adaptive 2x/3x/4x. Include both full-quality and Performance Mode where
  practical.
- Use at least three runs per case after warm-up. Record real FPS, displayed FPS, CPU and GPU frametime, p95/p99 or
  1%/0.1% lows, compositor missed frames, Adaptive diagnostics, generated-image fallback, and context recreation.
- Judge hitching from frametime distributions and traces, not the Steam FPS counter alone: that counter may combine real
  and generated output.
- Treat a change smaller than ordinary run-to-run variance as no measured gain.
- Reject a candidate for any reproducible flinch, temporary return to native cadence, new visual corruption, device
  loss, context churn, or recovery event that is absent from `aeae16f`, even if average performance is higher.

## Remaining user-facing improvements

- Expose a compact diagnostic summary: measured real FPS, displayed estimate, active generated-frame level, recovery
  state, and p95/p99. This would make target overshoot and micro-stutter reports reproducible.
- Complete the runtime compatibility matrix in `docs/Adaptive-Validation.md`, prioritizing Steam Deck DXVK and
  VKD3D-Proton, then AMD/Intel Wayland and NVIDIA.
- Define and test Fixed-multiplier capacity changes explicitly. Increasing Fixed multiplier while a game is running
  rebuilds the private context but may require a game restart to reserve enough game-owned swapchain images.
- Replace the native UI's detached polling save thread with a synchronized snapshot and bounded shutdown. This avoids a
  potential configuration data race and makes lifecycle behavior testable.
- Add native UI tests for profile creation/deletion, Active In editing, parser-range round trips, and empty-profile
  defaults.
- Before the next public build, bump `VERSION`, replace the `.21`-specific generated release notes, update the Decky
  immutable engine pin and integration ledger, and produce a fresh rollback package.

## Constraints to preserve

- Keep the generated-image timeout, history warm-up, menu/focus recovery, fast-cadence filtering, multiplier ceiling,
  and context-recreation guards intact.
- Do not add a runtime fallback or performance switch for these optimizations; each one should validate as the normal
  path in isolation or remain out of the release.
- Do not trade temporal-history correctness for benchmark numbers. History-only recovery work is intentional.
- Avoid shader/model approximations until profiling shows they are necessary and matched image-quality tests exist.
