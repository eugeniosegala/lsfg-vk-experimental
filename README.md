# lsfg-vk Experimental

<p align="center">
  <img src="assets/lsfg-vk-experimental-logo.png" alt="Experimental frame-generation mark for SteamOS and Linux" width="256" />
</p>

> **Experimental fork:** This repository carries independently developed features and fixes on top of the lsfg-vk
> `develop` branch. Builds can change quickly or regress for a particular game, compositor, driver, or GPU. Test each
> game independently and retain a known-good rollback. [UPSTREAM.md](UPSTREAM.md) records the reviewed baseline and the
> complete experimental history.

[Lossless Scaling](https://store.steampowered.com/app/993090/Lossless_Scaling/) is a Windows application that provides
scaling and frame-generation models. **lsfg-vk Experimental** is a Linux Vulkan layer that uses the frame-generation
model from the installed `Lossless.dll` to insert additional images between real game frames.

This fork packages the evolving lsfg-vk 2.x implementation. It does not include, modify, or replace `Lossless.dll`, and
it does not introduce a separate frame-generation model. For the established 1.x release line, use the
[stable lsfg-vk documentation](https://github.com/PancakeTAS/lsfg-vk/tree/ff1a0f72a7d6d08b84d58b7b4dc5f05c9f904f98).

## What this experimental build adds

- **Live Frame Generation toggle:** Stop and resume frame synthesis while a game is running without replacing the
  selected Fixed or Adaptive settings. Off mode directly presents the game's real frames and creates no per-swapchain
  interpolation context or images; the Vulkan layer and shared backend remain loaded so generation can resume live.
- **Adaptive Frame Generation:** Set a displayed-FPS target and let the Vulkan layer schedule between zero and three
  generated frames per real frame, up to a configurable 2x, 3x, or 4x ceiling. Fixed 2x, 3x, and 4x remain available
  and unchanged.
- **Load-aware adaptation:** Adaptive ramps generation gradually, retains proven levels, rolls back work that harms
  useful throughput, and waits for sustained evidence before trying a more expensive multiplier.
- **Optional Smooth Cadence:** Suitable fractional targets can prefer a validated constant interpolation cadence.
  This can look smoother but may lower real-frame cadence and responsiveness, so it is disabled by default.
- **HDR pipeline foundation (disabled by default in Decky):** Adds HDR10/PQ and linear-scRGB groundwork, not a promise
  of working HDR frame generation across games. Decky sets `LSFGVK_DISABLE_HDR_EXPOSURE=1` as the engine's hard SDR
  boundary and leaves DXVK at its normal SDR default; direct launchers can set the same LSFG variable. The engine
  activates HDR only
  after live application colour-space feedback or HDR metadata—not display capability alone—and uses real-frame
  passthrough for unsupported encodings. Validated HDR10 paths use packed 10-bit exchange images to reduce memory
  pressure while the model continues to work in linear 16-bit float.
- **Safe live mode transitions:** Fixed and Adaptive reserve one compatible private output set, so live generation,
  Fixed 2x/3x/4x (within the reserved capacity), Adaptive mode, target, ceiling, and Smooth Cadence can change without
  invalidating the game's swapchain. Backend-model and HDR-encoding changes wait for a natural game-owned recreation;
  a Decky setting never forces one.
- **64-bit and 32-bit Vulkan layers:** Host and Flatpak packages include architecture-matched layer libraries and
  manifests. The Vulkan loader selects the correct one for each game process; the CLI and Qt UI remain 64-bit.
- **SteamOS/Gamescope recovery:** Generated-image stalls fall back to real frames, keep temporal history current, and
  recover without repeatedly spending the full acquire timeout. Every recovery warms history in-place; LSFG never
  invalidates the game-owned swapchain to apply a recovery decision.
- **Menu and DX12 transition protection:** Adaptive preserves its proven gameplay state across hard cadence stalls and
  ignores implausibly fast DX12/VKD3D presentation bursts instead of treating them as a new game framerate.
- **Diagnostic tooling:** Opt-in, per-swapchain presentation records expose timing, recovery, ramp, rescue, and
  fast-cadence decisions without adding logging overhead to normal runs.
- **Experimental Flatpak extensions:** Dedicated runtime extensions for Freedesktop 23.08, 24.08, and 25.08 can coexist
  with the public `lsfgvk` extension.

See [Configuration](docs/Configuration.md) for the exact settings and controller limits, and
[Troubleshooting](docs/Troubleshooting.md) for diagnostic and recovery commands.
Developers can run the GPU-independent scheduler tests and use the runtime
[Adaptive validation matrix](docs/Adaptive-Validation.md) before publishing a build.
The current priorities, validation gates, and record of retired experiments are in
[Remaining improvements](docs/Remaining-Improvements.md).

### Adaptive Frame Generation quick start

Adaptive mode is opt-in. Configure it through `lsfg-vk-ui` or a profile in `~/.config/lsfg-vk/conf.toml`:

```toml
[[profile]]
name = "Adaptive 120 FPS"
active_in = ["Game.exe"]
adaptive = true
target_fps = 120
adaptive_max_multiplier = 3
adaptive_stable_cadence = false
frame_generation_enabled = true
```

Live generation, Fixed/Adaptive mode, Fixed multiplier, target, multiplier ceiling, and Smooth Cadence can be changed
while the game is running when the current context has the required reserved capacity. These changes reuse private
resources and do not ask the game to recreate its swapchain. Flow Scale, Performance Mode, GPU selection, a capacity
increase beyond the reserved set, and an HDR encoding change apply on the game's next natural swapchain recreation;
restart the game when an immediate deterministic change is required.

The target is an objective, not a guaranteed lock:

- Adaptive cannot reduce a game that already renders above the target. Apply a separate game or compositor cap when
  needed.
- It cannot exceed the selected multiplier ceiling or the available GPU/compositor throughput. If the real rate is too
  low, it deliberately remains below target rather than silently using a more artifact-prone ratio.
- Higher ratios and wider gaps between real frames can increase ghosting and input latency.
- Smooth Cadence may improve motion consistency on constrained hardware, but strict scheduling is usually more
  responsive. Leave it disabled unless a game benefits from the trade-off.

This scheduler is an independent Vulkan-layer implementation inspired by Lossless Scaling's
[Adaptive Frame Generation](https://store.steampowered.com/news/app/993090/view/518581441632666732). It is not a port of
the closed Windows capture engine and does not provide its Queue Target modes.

### SteamOS and Gamescope recovery

The [Decky LSFG-VK Experimental plugin](https://github.com/eugeniosegala/decky-lsfg-vk-experimental) applies the tested
50 ms generated-image timeout and in-place Adaptive recovery to games launched with its isolated wrapper. Direct
lsfg-vk users can opt in before their normal game command:

```bash
LSFGVK_PRESENT_ACQUIRE_TIMEOUT_MS=50 your-game-command
```

Adaptive recovery keeps the current game swapchain and warms temporal history in-place. Fixed mode uses the same
bounded acquisition fallback but resumes immediately when image acquisition succeeds. See
[Diagnosing presentation stalls](docs/Troubleshooting.md#diagnosing-presentation-stalls) for the full workflow.

## Installation

### Steam Deck and SteamOS

Use the [Decky LSFG-VK Experimental plugin](https://github.com/eugeniosegala/decky-lsfg-vk-experimental) for a private,
per-game installation that can coexist with the public Decky plugin. Follow that repository's installation, engine
update, launch-wrapper, and Heroic instructions instead of manually extracting this engine archive for the same game.

### Direct Linux installation

1. Purchase and install [Lossless Scaling](https://store.steampowered.com/app/993090/Lossless_Scaling/) through Steam.
2. Download the versioned Linux archive from this fork's
   [GitHub Releases](https://github.com/eugeniosegala/lsfg-vk-experimental/releases).
3. Extract it into your local prefix. For version `2.0.0-dev28-experimental.25`:

   ```bash
   tar -xJf lsfg-vk-2.0.0-dev28-experimental.25-linux.tar.xz -C ~/.local
   ```

Keep track of the extracted files so the direct installation can be removed or rolled back later.

This fork installs a uniquely named, wrapper-scoped implicit layer. Activate it for a direct game launch and suppress
either public LSFG identity if it is also installed:

```bash
ENABLE_LSFGVK_EXPERIMENTAL=1 DISABLE_LSFGVK=1 DISABLE_LSFG=1 your-game-command
```

For a Steam launch option, replace `your-game-command` with `%command%`. The experimental Decky plugin generates these
guards automatically; they affect only the wrapped game and do not uninstall or disable the public plugin globally.

The Linux archive contains both 64-bit and 32-bit Vulkan layer libraries. The
Vulkan loader selects the matching library for each game; no launcher-side
architecture setting is required. The CLI and graphical interface remain
64-bit applications.

The graphical interface requires Qt 6 and Qt Quick. Install the appropriate packages for your distribution if they are
not already available:

```bash
sudo apt install qt6-qpa-plugins libqt6quick6 qml6-module-qtquick-controls qml6-module-qtquick-layouts qml6-module-qtquick-window qml6-module-qtquick-dialogs qml6-module-qtqml-workerscript qml6-module-qtquick-templates qml6-module-qt-labs-folderlistmodel
sudo pacman -S qt6-declarative qt6-base
sudo dnf install qt6-qtdeclarative qt6-qtbase
```

Run only the command for your distribution. For sandboxed applications, install the extension matching the
application's Freedesktop runtime as described in the [Flatpak Guide](docs/Flatpak-Guide.md).

## Usage

### Graphical configuration

Open **lsfg-vk Configuration Window** from the application launcher or run:

```bash
~/.local/bin/lsfg-vk-ui
```

Create or select a profile, configure its frame-generation mode, and add the target executable or process under
**Active In**. Global settings apply to every profile, including a custom `Lossless.dll` path.

### Manual configuration

The default configuration is `~/.config/lsfg-vk/conf.toml`; it is created when a Vulkan application first loads the
layer. Profiles are stored as `[[profile]]` sections and can match Linux binaries, Windows executables, process names,
or a trailing executable path through `active_in`.

Validate the configuration with:

```bash
~/.local/bin/lsfg-vk-cli validate
```

Detailed field descriptions and environment-variable equivalents are in
[Configuration](docs/Configuration.md).

### Benchmarking

Run the built-in frame-generation benchmark with:

```bash
~/.local/bin/lsfg-vk-cli benchmark
```

The default duration is 10 seconds. Add `-h` to list the available options.

## 🎮 In-game considerations

> [!TIP]
> **Try the game's V-Sync setting first.** In many games it can materially improve frame pacing and the perceived
> smoothness of frame generation. Test it both enabled and disabled before making deeper adjustments.

Every game, renderer, and display setup behaves differently. Also compare Fixed and Adaptive Frame Generation, then
fullscreen, borderless, and windowed modes. Change one setting at a time, restart after major display or
frame-generation changes, and keep the configuration that feels best for that game.

## Build and publish a release

This repository builds and publishes locally without GitHub Actions. Install the dependencies from
[Building from Source](docs/Building-From-Source.md). On macOS, start Docker Desktop; the packaging scripts build the
dual-architecture Vulkan layers and 64-bit CLI/UI inside a local `linux/amd64` container.

Create and verify a local host archive without changing GitHub:

```bash
scripts/package-local.sh
```

Build the Flatpak runtime-extension archive when required:

```bash
scripts/package-flatpaks.sh
```

To publish the version declared in [`VERSION`](VERSION), commit a clean `develop` branch, authenticate `gh`, and run:

```bash
scripts/publish-package.sh
```

The publish script builds and verifies the host and Flatpak archives, calculates their SHA-256 checksums, creates and
pushes an annotated version tag, and publishes a GitHub prerelease with generated notes. Bump `VERSION` before each new
release.

## Credits

- **[PancakeTAS](https://github.com/PancakeTAS/lsfg-vk)** for creating the lsfg-vk Vulkan compatibility layer
- **[Lossless Scaling](https://store.steampowered.com/app/993090/Lossless_Scaling/)** developers for the original
  frame-generation technology
