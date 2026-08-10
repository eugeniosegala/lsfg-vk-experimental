# lsfg-vk Experimental

<p align="center">
  <img src="assets/lsfg-vk-experimental-logo.png" alt="Experimental frame-generation mark for SteamOS and Linux" width="256" />
</p>

> **Experimental fork:** This repository builds independently developed experimental changes on top of the lsfg-vk
> `develop` branch, with the explicit goal of pushing the library to its limits. Builds can change rapidly, regress
> for particular games or drivers, and should be tested per game rather than assumed to replace a known-good installation.
> See [UPSTREAM.md](UPSTREAM.md) for the reviewed upstream baseline and every carried change.

**Lossless Scaling** is a Windows-exclusive program featuring various algorithms for scaling and interpolating programs.

**lsfg-vk Experimental** is a Vulkan layer that hooks into Vulkan applications and generates additional frames using
Lossless Scaling's frame generation algorithm.

For the established 1.x release line, use
the [stable lsfg-vk documentation](https://github.com/PancakeTAS/lsfg-vk/tree/ff1a0f72a7d6d08b84d58b7b4dc5f05c9f904f98).
Keep a working configuration handy when testing this experimental fork.

## What is this?

This repository packages the evolving 2.x lsfg-vk implementation for people who specifically want to test new Vulkan
frame-generation work. It remains the same Linux compatibility layer and still uses the `Lossless.dll` installed by the
Lossless Scaling Steam application; it is not a separate frame-generation algorithm.

The experimental status matters: compatibility depends on the game, compositor, GPU driver, and selected options. Please
test changes one game at a time and include the build version, GPU/driver, and game details in any report.

The experimental line also includes an opt-in Adaptive Frame Generation scheduler, inspired by Lossless Scaling's
[Adaptive Frame Generation](https://store.steampowered.com/news/app/993090/view/518581441632666732). It varies
fractional interpolation outputs toward a configured target while retaining the existing Fixed mode. This is an
independent Vulkan-layer implementation, not a port of the closed Windows capture engine: it can add frames up to a 4x
ceiling, but it cannot reduce a native framerate already above the target or provide the Windows Queue Target modes.
After startup or a presentation disruption, it stabilizes on real frames and ramps generation gradually; if a higher
step harms useful throughput, it temporarily falls back to the previous step. When Gamescope's cadence divisor makes
the first generated-frame step look counterproductive, the scheduler may make one bounded bridge test at the next
step. When strict scheduling already needs nearly every slot at an integer cadence, it can briefly validate constant
generation rather than alternating generated and real-only frames. Strict scheduling settles first, and the constant
cadence is retained only while it continues to meet the target with sufficient base-rate headroom. If that validated
cadence later suffers a severe sustained collapse, Adaptive measures the real-only rate for one second, then resumes
fractional scheduling or probes one higher multiplier when the configured maximum permits it. Rescue attempts have a
15-second cooldown and never exceed `adaptive_max_multiplier`. Set `adaptive_stable_cadence = false` to use strict
target scheduling instead while retaining the other Adaptive protections. Repeated failures at a higher multiplier use a
progressive cooldown, while a meaningful base-rate improvement permits an earlier retry. After a generated-image
recovery, the existing warm-up is retained but Adaptive resumes from its last validated generation level instead of
ramping blindly from zero. Adaptive policy evaluation is frozen while generated output is bypassed, preventing the
real-frame-only recovery period from falsely validating a multiplier. Abrupt menu, focus, or display transitions also
retain the pre-transition base-rate baseline and proven generation level. Adaptive waits for one second of real-only
cadence at least 90% of that baseline before restoring the proven level; if cadence does not recover within five
seconds, it discards the stale baseline and performs a clean ramp. The first generated-image recovery during this
window uses history warm-up without forcing a swapchain rebuild, leaving the guarded rebuild as a second-stage fallback.
See [Configuration](docs/Configuration.md) for the exact limits.

### SteamOS / Gamescope recovery override

The guarded swapchain-rebuild stage is intentionally controlled by an environment variable. It applies only to
Adaptive mode, and only after LSFG-VK has recovered from a genuine generated-image acquisition stall. During a detected
menu or focus discontinuity, the first recovery uses a soft history warm-up; a later stall can still request the
rebuild. This can clear presentation latency left behind by repeated Steam-menu transitions, but a small number of
games may pause, flicker, or handle a swapchain rebuild poorly.

The Decky experimental plugin enables the tested 50 ms bounded acquisition timeout and guarded rebuild automatically.
For direct lsfg-vk use, enable both before your game command:

```bash
LSFGVK_PRESENT_ACQUIRE_TIMEOUT_MS=50 LSFGVK_PRESENT_RECOVERY_RECREATE=1 your-game-command
```

If a specific game does not tolerate the rebuild, retain the bounded timeout and history-only recovery while disabling
only the rebuild:

```bash
LSFGVK_PRESENT_ACQUIRE_TIMEOUT_MS=50 LSFGVK_PRESENT_RECOVERY_RECREATE=0 your-game-command
```

## Installation

If you are on a Steam Deck or similar handheld, consider
the [Decky LSFG-VK Experimental plugin](https://github.com/eugeniosegala/decky-lsfg-vk-experimental). It installs its
own private experimental layer and per-game launcher, so it can coexist with the public Decky LSFG-VK plugin. The Decky
plugin is independently maintained; direct plugin questions to its repository and community support channels. If you use
the plugin for a game, follow its installation guide and launcher instructions instead of manually installing this archive
for that game.

1. Before proceeding, please make sure you
   have [Lossless Scaling](https://store.steampowered.com/app/993090/Lossless_Scaling/) downloaded on Steam. For an
   experimental build, keep a rollback path to a previously working release.
2. Download the Linux archive from this
   fork's [GitHub Releases](https://github.com/eugeniosegala/lsfg-vk-experimental/releases). The archive name includes
   its exact experimental version, for example `lsfg-vk-2.0.0-dev28-experimental.1-linux.tar.xz`.
3. Open a terminal in the folder where you downloaded the file and run the following:

```bash
tar -xJf lsfg-vk-2.0.0-dev28-experimental.1-linux.tar.xz -C ~/.local
```

This will extract lsfg-vk to `~/.local`. Please **keep track of the files that were extracted**, in case you want to
uninstall lsfg-vk later.

4. The graphical interface requires Qt6 and Qt6 Quick in order to run. If you do not have these installed, install the
   following packages:

```bash
sudo apt install qt6-qpa-plugins libqt6quick6 qml6-module-qtquick-controls qml6-module-qtquick-layouts qml6-module-qtquick-window qml6-module-qtquick-dialogs qml6-module-qtqml-workerscript qml6-module-qtquick-templates qml6-module-qt-labs-folderlistmodel # On Debian/Ubuntu-based systems
sudo pacman -S qt6-declarative qt6-base # On Arch-based systems
sudo dnf install qt6-qtdeclarative qt6-qtbase # On Fedora
```

5. (Optional) If you wish to use lsfg-vk within Flatpak applications, see the [Flatpak Guide](docs/Flatpak-Guide.md).

## Package and publish a release

Releases are made locally with scripts; this repository does not use GitHub Actions or CI to build or publish them. On
Linux, the scripts build directly. On macOS, `package-local.sh` uses a local `linux/amd64` Docker container
automatically; install Docker Desktop and start it first. The archive always targets 64-bit Linux.

Install the build dependencies described in [Building from Source](docs/Building-From-Source.md), then create an archive
for local testing:

```bash
scripts/package-local.sh
```

This creates `out/lsfg-vk-experimental-linux.tar.xz`. It builds and verifies the Vulkan layer, CLI, UI, manifest, and
XDG files, but does not create a tag, upload anything, or change GitHub.

To publish the version in [`VERSION`](VERSION), first commit a clean `develop` branch, authenticate the GitHub CLI with
`gh auth login -h github.com`, then run:

```bash
scripts/publish-package.sh
```

The publish script builds `out/lsfg-vk-<VERSION>-linux.tar.xz`, records its SHA-256 in generated release notes, creates
an annotated `v<VERSION>` tag, pushes `develop` and the tag, and publishes a GitHub prerelease with the archive
attached. Bump `VERSION` before every subsequent release.

## Usage

In order to start using lsfg-vk, you will need to configure it. This can either be done using the GUI application, or
manually.

### Graphical Configuration

Start 'lsfg-vk Configuration Window' from your application launcher, or run `~/.local/bin/lsfg-vk-ui` in a terminal:

- On the left side, you will see a list of profiles. Each profile has its own settings.
- All properties in the "Global Settings" section apply to all profiles.
    - Should Lossless Scaling be installed in a non-standard location, you can specify the path here.
- Select a profile and configure the "Profile Settings" section to your liking.
    - When editing the "Active In" list, you can add a game using its executable name (e.g. `Game.exe`, `mpv`).
- Please see the [documentation](docs/Configuration.md) for detailed information on each setting.
- Once you are done configuring, simply starting a game that matches one of the profiles will automatically apply the
  settings.

### Manual Configuration

The default configuration is located in `~/.config/lsfg-vk/conf.toml`. It will be created automatically when any Vulkan
application is started.

- In the `[global]` section, you can change where Lossless Scaling is installed, as well as other global settings.
- Each profile is defined in its own `[[profile]]` section.
- The `active_in` array/string defines which applications the profile is active in. You can add applications using their
  executable name (e.g. `Game.exe`, `mpv`).
- Please see the [documentation](docs/Configuration.md) for detailed information on each setting.
- Once you are done configuring, simply starting a game that matches one of the profiles will automatically apply the
  settings.

You can validate the configuration using `lsfg-vk-cli`:

```bash
~/.local/bin/lsfg-vk-cli validate
```

### Benchmarking Mode

You can run a frame generation benchmark using `lsfg-vk-cli`:

```bash
~/.local/bin/lsfg-vk-cli benchmark
```

By default, the benchmark will run for 10 seconds. Add `-h` to see all available benchmarking options.

## Credits

- **[PancakeTAS](https://github.com/PancakeTAS/lsfg-vk)** for creating the lsfg-vk Vulkan compatibility layer
- **[Lossless Scaling](https://store.steampowered.com/app/993090/Lossless_Scaling/)** developers for the original frame
  generation technology
