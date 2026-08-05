# lsfg-vk Experimental

<p align="center">
  <img src="assets/lsfg-vk-experimental-logo.png" alt="Experimental frame-generation mark for SteamOS and Linux" width="256" />
</p>

> **Experimental fork:** This repository is an experimental fork of the lsfg-vk development line. It tracks work
> intended for testing before it is treated as a stable release. Builds can change rapidly, regress for particular
> games or drivers, and should be tested per game rather than assumed to replace a known-good installation.

**Lossless Scaling** is a Windows-exclusive program featuring various algorithms for scaling and interpolating programs.

**lsfg-vk Experimental** is a Vulkan layer that hooks into Vulkan applications and generates additional frames using
Lossless Scaling's frame generation algorithm.

For the established 1.x release line, use the [stable lsfg-vk documentation](https://github.com/PancakeTAS/lsfg-vk/tree/ff1a0f72a7d6d08b84d58b7b4dc5f05c9f904f98). Keep a working configuration handy when testing this experimental fork.

## What is this?

This repository packages the evolving 2.x lsfg-vk implementation for people who specifically want to test new Vulkan
frame-generation work. It remains the same Linux compatibility layer and still uses the `Lossless.dll` installed by
the Lossless Scaling Steam application; it is not a separate frame-generation algorithm.

The experimental status matters: compatibility depends on the game, compositor, GPU driver, and selected options.
Please test changes one game at a time and include the build version, GPU/driver, and game details in any report.

## Installation

If you are on a Steam Deck or similar handheld, consider the [Decky LSFG-VK Experimental plugin](https://github.com/eugeniosegala/decky-lsfg-vk-experimental). It installs its own private experimental layer and per-game launcher, so it can coexist with the public Decky LSFG-VK plugin. The Decky plugin is independently maintained; direct plugin questions to its repository and community support channels.

1. Before proceeding, please make sure you have [Lossless Scaling](https://store.steampowered.com/app/993090/Lossless_Scaling/) downloaded on Steam. For an experimental build, keep a rollback path to a previously working release.
2. Download the Linux archive from this fork's [GitHub Releases](https://github.com/eugeniosegala/lsfg-vk-experimental/releases). The archive name includes its exact experimental version, for example `lsfg-vk-2.0.0-dev28-experimental.1-linux.tar.xz`.
3. Open a terminal in the folder where you downloaded the file and run the following:
```bash
tar -xJf lsfg-vk-2.0.0-dev28-experimental.1-linux.tar.xz -C ~/.local
```
This will extract lsfg-vk to `~/.local`. Please **keep track of the files that were extracted**, in case you want to uninstall lsfg-vk later.

4. The graphical interface requires Qt6 and Qt6 Quick in order to run. If you do not have these installed, install the following packages:
```bash
sudo apt install qt6-qpa-plugins libqt6quick6 qml6-module-qtquick-controls qml6-module-qtquick-layouts qml6-module-qtquick-window qml6-module-qtquick-dialogs qml6-module-qtqml-workerscript qml6-module-qtquick-templates qml6-module-qt-labs-folderlistmodel # On Debian/Ubuntu-based systems
sudo pacman -S qt6-declarative qt6-base # On Arch-based systems
sudo dnf install qt6-qtdeclarative qt6-qtbase # On Fedora
```

5. (Optional) If you wish to use lsfg-vk within Flatpak applications, see the [Flatpak Guide](docs/Flatpak-Guide.md).

## Package and publish a release

Releases are made locally with scripts; this repository does not use GitHub Actions or CI to build or publish them.
On Linux, the scripts build directly. On macOS, `package-local.sh` uses a local `linux/amd64` Docker container automatically; install Docker Desktop and start it first. The archive always targets 64-bit Linux.

Install the build dependencies described in [Building from Source](docs/Building-From-Source.md), then create an archive for local testing:

```bash
scripts/package-local.sh
```

This creates `out/lsfg-vk-experimental-linux.tar.xz`. It builds and verifies the Vulkan layer, CLI, UI, manifest, and XDG files, but does not create a tag, upload anything, or change GitHub.

To publish the version in [`VERSION`](VERSION), first commit a clean `develop` branch, authenticate the GitHub CLI with `gh auth login -h github.com`, then run:

```bash
scripts/publish-package.sh
```

The publish script builds `out/lsfg-vk-<VERSION>-linux.tar.xz`, records its SHA-256 in generated release notes, creates an annotated `v<VERSION>` tag, pushes `develop` and the tag, and publishes a GitHub prerelease with the archive attached. Bump `VERSION` before every subsequent release.

## Usage
In order to start using lsfg-vk, you will need to configure it. This can either be done using the GUI application, or manually.

### Graphical Configuration
Start 'lsfg-vk Configuration Window' from your application launcher, or run `~/.local/bin/lsfg-vk-ui` in a terminal:
- On the left side, you will see a list of profiles. Each profile has its own settings.
- All properties in the "Global Settings" section apply to all profiles.
  - Should Lossless Scaling be installed in a non-standard location, you can specify the path here.
- Select a profile and configure the "Profile Settings" section to your liking.
  - When editing the "Active In" list, you can add a game using its executable name (e.g. `Game.exe`, `mpv`).
- Please see the [documentation](docs/Configuration.md) for detailed information on each setting.
- Once you are done configuring, simply starting a game that matches one of the profiles will automatically apply the settings.

### Manual Configuration
The default configuration is located in `~/.config/lsfg-vk/conf.toml`. It will be created automatically when any Vulkan application is started.
- In the `[global]` section, you can change where Lossless Scaling is installed, as well as other global settings.
- Each profile is defined in its own `[[profile]]` section.
- The `active_in` array/string defines which applications the profile is active in. You can add applications using their executable name (e.g. `Game.exe`, `mpv`).
- Please see the [documentation](docs/Configuration.md) for detailed information on each setting.
- Once you are done configuring, simply starting a game that matches one of the profiles will automatically apply the settings.

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

## Support and Troubleshooting
If you encounter any issues or have questions regarding lsfg-vk, read through the [Troubleshooting](docs/Troubleshooting.md) documentation page or join the [Discord server](https://discord.gg/losslessscaling) for assistance.
