/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include "lsfg-vk-backend/lsfgvk.hpp"

#include <string_view>

#include <vulkan/vulkan_core.h>

namespace lsfgvk::layer {

    /// Colour handling selected from the complete Vulkan surface-format pair.
    struct SwapchainColorPipeline {
        backend::FrameEncoding encoding{backend::FrameEncoding::Sdr8};
        VkFormat exchangeFormat{VK_FORMAT_R8G8B8A8_UNORM};
        bool generationSupported{true};
        bool hdr{false};
        std::string_view name{"sdr-8-bit"};
        std::string_view reason{};
    };

    /// Classify a swapchain without relying on VkFormat enum ordering.
    [[nodiscard]] SwapchainColorPipeline classifySwapchainColor(
        VkFormat format, VkColorSpaceKHR colorSpace
    );

}
