# Troubleshooting
This page documents common issues, known incompatibilities and contains a guide to help you create a helpful bug report.

Before reporting a bug, please read through the following sections to see if your issue is already addressed.

### Basic Troubleshooting Steps
If lsfg-vk does not seem to be doing *anything*:
- Ensure the game you are trying to run is using Vulkan (not OpenGL).
- Ensure you are running a 64-bit game (try `PROTON_USE_WOW64=1`, but if it doesn't work then you're out of luck).
- Install `vulkan-tools` and run `vulkaninfo | grep -i VK_LAYER_LSFGVK_frame_generation`.
  - If there is no output revisit the installation steps.
- Launch the game with the environment variable `VK_LOADER_DEBUG=layer` set.
  - Look for lines mentioning `VK_LAYER_LSFGVK_frame_generation` inbetween `<Loader>` and `<Device>`.
  - If you can't find any, try again using `LSFGVK_ENV=1`.
    - If it still doesn't show up, you may be running in flatpak.
    - If it does show up, then the `active_in` property of your profile is likely misconfigured. Reconfigure it, then try again without `LSFGVK_ENV=1`.
- Check for warnings/errors from lsfg-vk in the terminal/log output. These will often give clues as to what is going wrong.
- If there are no errors/warnings and you have gone through all above steps, then move onto the next section.

If lsfg-vk is loaded, but frame generation is not working:
- (When using `pacing_mode = none`): Disable VRR.
- (When using `pacing_mode = none`): Explicitly enable V-Sync in your game settings.
- (When using `pacing_mode = none` on Gamescope/SteamDeck): Set `ENABLE_GAMESCOPE_WSI=0`.
- (When using `pacing_mode = none` on Wayland): Disable tearing control & direct passthrough in your compositor
- (When using `pacing_mode = none` on Wayland): Try running in windowed mode.
- Disable in-game upscaling options (e.g. DLSS, FSR, etc).
- Disable other Vulkan layers (e.g. VkBasalt, MangoHud)

If games do not open at all with lsfg-vk enabled for them (stuck at black screen):
- Ensure you configured the correct `gpu` for this profile, in case you have multiple GPUs and/or drivers (lsfg-vk-ui will show all available GPUs in a dropdown), lsfg-vk might be defaulting to a different one than the game is using

Should none of the above help, please proceed to the bug reporting section.

### Performance Overlays
If you are using performance overlays like Steam's built-in overlay, there is a good chance that they will not show the correct framerate.

This is a known limitation of Vulkan layers and without directly working with the overlay developers, there is little that can be done to fix this.

### Opening a Bug Report
When opening a bug report, please include the following information to help us diagnose and fix the issue:
- A detailed description of the issue you are experiencing.
- What system you are running on (OS, GPU, drivers, etc).
- The game you are trying to run (and through what platform, e.g. Steam Proton, native Linux, etc).
- The relevant section of your lsfg-vk configuration file.

Ideally, also include a log file with the environment variables `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` and `VK_LOADER_DEBUG=all` set. You might need to install the Vulkan validation layers package for your distribution to do this.

If you're running the game through Steam, the log file is located at `~/.steam/steam/logs/console-linux.txt`. Please clear it before launching the game to ensure it only contains relevant information.

### Diagnosing presentation stalls

Experimental builds can log Vulkan presentation operations that take longer than expected. This is intended for
targeted debugging and is disabled by default, so it has no effect on normal runs.

Add `LSFGVK_PRESENT_DIAGNOSTICS=1` before the normal launch command. Operations taking at least 20 ms are written to
Steam's `~/.steam/steam/logs/console-linux.txt` log. Override the threshold in milliseconds with
`LSFGVK_PRESENT_DIAGNOSTICS_THRESHOLD_MS`.

When using the isolated Decky LSFG-VK Experimental plugin, keep its wrapper in the launch option:

```bash
LSFGVK_PRESENT_DIAGNOSTICS=1 LSFGVK_PRESENT_DIAGNOSTICS_THRESHOLD_MS=25 ~/.local/bin/lsfg-vk-experimental %command%
```

To test recovery from a stalled generated-image acquisition, add an opt-in timeout in milliseconds. If the timeout is
reached, lsfg-vk skips the remaining generated frames for that presentation and safely presents the original game
frame. Following attempts probe image availability before scheduling output passes, so a Gamescope overlay cannot impose
the full timeout or waste GPU work on generated frames that cannot be presented. A shared history-only pre-pass still
updates the model's temporal features for every real frame instead of leaving older slots untouched.
If zero-timeout probes keep missing
the release window, the layer makes one bounded reacquisition attempt per second after the first second of fallback.
The real game frames continue updating the two source images. Fixed mode resumes automatically as soon as a probe
succeeds. By default, Adaptive mode first presents three real frames while repopulating its deepest temporal-history
ring, then attempts generated output again. The same three-frame warm-up runs when an Adaptive context first starts.
For example:

```bash
LSFGVK_PRESENT_ACQUIRE_TIMEOUT_MS=25 LSFGVK_PRESENT_DIAGNOSTICS=1 LSFGVK_PRESENT_DIAGNOSTICS_THRESHOLD_MS=25 ~/.local/bin/lsfg-vk-experimental %command%
```

This recovery is experimental and disabled when `LSFGVK_PRESENT_ACQUIRE_TIMEOUT_MS` is absent or set to `0`.
With diagnostics enabled, `skip-generated-frames` reports whether the fallback followed the initial timeout, a
non-blocking retry, or a periodic `bounded-retry`. Its `backend_work` field records whether full output work was already
`scheduled` or only the temporal `history-only` pre-pass ran. Expected repeated non-blocking failures are aggregated;
Fixed-mode recovery uses `resume-generated-frames`. Adaptive recovery reports `generated-image-recovered`, followed by
three `history-warmup` entries with `reason=recovery`. Adaptive startup uses the same entries with `reason=startup`.
The recovery record includes the total number of frames whose output work was bypassed.

If repeated overlay transitions still accumulate input latency, the experimental Adaptive recovery can instead ask
the game to rebuild its Vulkan swapchain after Gamescope releases an image:

```bash
LSFGVK_PRESENT_ACQUIRE_TIMEOUT_MS=50 LSFGVK_PRESENT_RECOVERY_RECREATE=1 ~/.local/bin/lsfg-vk-experimental %command%
```

This request is made only after a real acquire timeout and a later successful recovery probe. The acquired image is
safely presented before lsfg-vk returns `VK_ERROR_OUT_OF_DATE_KHR`, the standard signal applications use to recreate
their swapchain. Diagnostics report `generated-image-recovered recovery_action=swapchain-recreate`, followed by
`request-swapchain-recreation`. The replacement context stabilizes on real frames before generated-frame load is
ramped one step at a time. A five-second cross-context cooldown suppresses immediate recreation loops. A short pause
or flicker can occur while the game rebuilds its swapchain. Some games may mishandle a forced recreation; set
`LSFGVK_PRESENT_RECOVERY_RECREATE=0` for that game to return to the history-only recovery. Fixed mode is unaffected.

For a normal non-isolated installation, place the same environment variables before its usual launch command.

Clear the Steam log before reproducing the problem. After reproducing it, extract the most recent diagnostic entries
with:

```bash
grep -aE 'lsfg-vk: present diagnostics: operation=(adaptive-stabilization|adaptive-ramp|adaptive-ramp-accepted|adaptive-load-shed|skip-generated-frames|generated-image-recovered|request-swapchain-recreation|swapchain-recreation-suppressed|swapchain-context-create|swapchain-context-destroy)' ~/.steam/steam/logs/console-linux.txt | tail -n 800
```

`adaptive-stabilization` and `adaptive-ramp` show the normal restart sequence. `adaptive-load-shed` means a tested
multiplier reduced useful throughput and was rolled back. `swapchain-recreation-suppressed` confirms the cooldown
prevented a repeated recreation request.

Disable the diagnostic variables after collecting the trace. Remove the acquire-timeout and recreation variables too
if you do not want to continue testing the recovery path.
