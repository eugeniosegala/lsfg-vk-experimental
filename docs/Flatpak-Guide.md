# Flatpak Guide

If you want to use **lsfg-vk** with Flatpak applications, you must install the Vulkan layer for Flatpak. You can also optionally install the graphical configuration editor **lsfg-vk-ui** as a Flatpak application.

## Installation

You can install lsfg-vk for Flatpak through three different methods.

### From an experimental release

Experimental releases include a `flatpaks.tar.xz` archive containing one runtime extension for each supported
Freedesktop runtime (23.08, 24.08, and 25.08). Extract the archive and install the extension matching the
application's runtime:

```bash
tar -xJf lsfg-vk-<version>-flatpaks.tar.xz
flatpak install --user org.freedesktop.Platform.VulkanLayer.lsfgvkexperimental-24.08.flatpak
```

This fork intentionally uses the separate `org.freedesktop.Platform.VulkanLayer.lsfgvkexperimental` extension ID.
It can therefore remain installed beside the public Flathub `lsfgvk` extension without overwriting it.

### Through Custom Build

If you want to build lsfg-vk yourself, install `flatpak-builder` and run the following commands:
```bash
git clone --depth=1 https://github.com/eugeniosegala/lsfg-vk-experimental.git
# optional: git checkout <desired-version>
cd lsfg-vk-experimental
flatpak-builder --force-clean --user --install-deps-from=flathub --install flatpak-build \
    dist/flatpak/lsfg-vk-ui/gay.pancake.lsfg-vk-ui.yml
flatpak-builder --force-clean --user --install-deps-from=flathub --install flatpak-build \
    dist/flatpak/lsfg-vk-layer/org.freedesktop.Platform.VulkanLayer.lsfgvkexperimental_23.08.yml
flatpak-builder --force-clean --user --install-deps-from=flathub --install flatpak-build \
    dist/flatpak/lsfg-vk-layer/org.freedesktop.Platform.VulkanLayer.lsfgvkexperimental_24.08.yml
flatpak-builder --force-clean --user --install-deps-from=flathub --install flatpak-build \
    dist/flatpak/lsfg-vk-layer/org.freedesktop.Platform.VulkanLayer.lsfgvkexperimental_25.08.yml
```

## Configuration

Before using lsfg-vk with Flatpak applications, you need to give them access to the configuration directory, as well as Lossless Scaling.
```bash
export appid=  # e.g. io.mpv.Mpv
mkdir -p ~/.config/lsfg-vk
flatpak override --user --filesystem=/home/$USER/.config/lsfg-vk:rw $appid
flatpak override --user --filesystem=/home/$USER/local/share/Steam/steamapps/common:ro $appid
flatpak override --user --env=LSFGVK_CONFIG=/home/$USER/.config/lsfg-vk/conf.toml $appid
```
