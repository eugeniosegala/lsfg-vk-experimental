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
| Current experimental commit | [`82e0d499`](https://github.com/eugeniosegala/lsfg-vk-experimental/commit/82e0d49976db8bce5e472fa154526e971529e091) |
| Reviewed on                 | 2026-08-05                                                                                                          |

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
