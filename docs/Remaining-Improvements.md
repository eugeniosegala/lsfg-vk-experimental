# LSFG-VK improvement roadmap

This is a new, ordered backlog for the experimental branch. It starts from the
known-good `experimental.22` recovery checkpoint; it is not a record of prior
experiments. Every performance candidate must be measured against that
checkpoint, tested in isolation, and rejected for any reproducible quality,
stability, or recovery regression.

## Current local tester: submission bookkeeping

- Keep Vulkan submission metadata in inline storage for the normal one-to-three
  semaphore path, while retaining a dynamic fallback for an application that
  supplies more present-wait semaphores.
- Preserve semaphore order, timeline values, wait-stage masks, fences, command
  buffers, and all queue-submission ordering exactly. This is a CPU/driver
  overhead reduction only; it intentionally does not alter shaders, barriers,
  interpolation timestamps, generated-frame counts, or Adaptive policy.
- Validate this candidate with the focused submission-layout tests, the
  deterministic Adaptive tests and matrix, then a real-device A/B run. Expect
  a modest frametime-consistency improvement where submission overhead matters,
  not an unqualified ten-percent GPU gain.

## Measurement before more optimization

- Add opt-in CPU timing around command recording, queue submission, acquire,
  and present. Keep the diagnostics allocation-free on the hot path and log
  summaries rather than per-frame output.
- Add opt-in Vulkan timestamp queries around the pre-pass and generated-output
  work so a game can be classified as GPU compute, memory bandwidth, driver,
  compositor, or CPU limited before changing it.
- Define a repeatable A/B harness: same scene, power profile, refresh rate,
  game settings, LSFG mode, and warm-up. Record real FPS, displayed FPS, CPU
  and GPU frametime, p95/p99, missed presents, generated-image recovery, and
  context recreation.
- Test Fixed 2x/3x/4x and Adaptive ceilings 2x/3x/4x, with Smooth Cadence both
  off and on. Treat a result inside normal run-to-run variance as no gain.

## Low-risk performance work

- Evaluate persistent mapping for frequently updated coherent buffers as a
  separate, guarded candidate after the submission build is proven. Confirm
  every host write is fenced from the prior GPU read on AMD, Intel, and NVIDIA.
- Reduce avoidable temporary allocations and repeated host-side setup in
  presentation and scheduling paths only after a profiler identifies them.
- Reuse scratch images or buffers only when lifetime analysis proves that no
  in-flight command buffer or context recreation can observe stale storage.
- Keep the current image blit compatibility path unless format and usage flags
  prove a faster copy path is legal for all supported swapchains.

## GPU cost and image quality

- Profile shader dispatch cost by pass and resolution before attempting any
  model, shader, precision, or resource-layout change. A real GPU gain needs
  less measured work or better overlap; it cannot be assumed from shorter host
  code.
- Do not change motion vectors, interpolation timestamps, model selection, or
  generated-frame count to claim a performance win without matched visual
  captures. These are direct ghosting and temporal-stability risks.
- Create a visual regression scene set with camera pans, thin geometry, HUD,
  particles, menus, and scene cuts. Compare captures at matching real cadence
  and multiplier before making ghosting claims.

## Adaptive and presentation reliability

- Continue to expand deterministic state-machine tests for menu/focus changes,
  abrupt real-FPS shifts, target overshoot above 90 FPS, recovery re-entry, and
  Smooth Cadence at 2x/3x/4x.
- Maintain a compact compatibility matrix covering Steam Deck DXVK and
  VKD3D-Proton first, then AMD/Intel Wayland and NVIDIA. Include gamescope,
  overlays, HDR-off operation, and Flatpak/Heroic where applicable.
- Keep the generated-image timeout, history warm-up, bounded reacquire,
  menu/focus recovery, fast-cadence filter, multiplier ceiling, and
  context-recreation cooldown guards in every candidate build.
- Add a user-visible diagnostic summary with measured real FPS, displayed
  estimate, active generation level, fallback/recovery state, and recent
  p95/p99 frametime so reports can be reproduced.

## Engineering and release gates

- Keep scheduler decisions deterministic and unit-testable; new timing policy
  must accept an injected clock and have tests for both transitions and
  invariants.
- Keep public APIs and user configuration backwards compatible unless a tested
  migration is supplied.
- Build and run the full Linux test suite, validate the host archive contents,
  and perform a clean local plugin install before requesting hardware testing.
- Publish only after the candidate passes the matrix and A/B evidence shows a
  repeatable benefit without new fallback, flinch, crash, device-loss, visual,
  or pacing regression. Retain the prior working local archive for rollback.
