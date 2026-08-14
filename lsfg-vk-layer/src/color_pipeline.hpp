/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include "lsfg-vk-backend/lsfgvk.hpp"

#include <cstddef>
#include <string_view>

#include <vulkan/vulkan_core.h>

namespace lsfgvk::layer {

    /// Colour handling selected from the complete Vulkan surface-format pair.
    struct SwapchainColorPipeline {
        backend::FrameEncoding encoding{backend::FrameEncoding::Sdr8};
        VkFormat exchangeFormat{VK_FORMAT_R8G8B8A8_UNORM};
        bool generationSupported{true};
        bool hdr{false};
        bool gamescopeColorSpaceRecovered{false};
        bool packedHdr10Transport{false};
        std::string_view name{"sdr-8-bit"};
        std::string_view reason{};
    };

    /// Classify a swapchain without relying on VkFormat enum ordering.
    [[nodiscard]] SwapchainColorPipeline classifySwapchainColor(
        VkFormat format, VkColorSpaceKHR colorSpace,
        bool gamescopeHdrActive = false
    );

    /// Select the compact HDR10 boundary representation only when the game
    /// device can export it and the backend device can import/write it.
    [[nodiscard]] bool enablePackedHdr10Transport(
        SwapchainColorPipeline& pipeline,
        bool applicationDeviceSupported,
        bool backendDeviceSupported
    );

    [[nodiscard]] size_t transportBytesPerPixel(
        backend::FrameEncoding encoding
    );

}
