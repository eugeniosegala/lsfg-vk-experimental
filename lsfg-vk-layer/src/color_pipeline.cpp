/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "color_pipeline.hpp"

using namespace lsfgvk;

namespace {
    bool isEightBitSdrFormat(const VkFormat format) {
        switch (format) {
            case VK_FORMAT_R8G8B8A8_UNORM:
            case VK_FORMAT_R8G8B8A8_SRGB:
            case VK_FORMAT_B8G8R8A8_UNORM:
            case VK_FORMAT_B8G8R8A8_SRGB:
                return true;
            default:
                return false;
        }
    }

    bool isHighPrecisionSdrFormat(const VkFormat format) {
        switch (format) {
            case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
            case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
            case VK_FORMAT_R16G16B16A16_SFLOAT:
                return true;
            default:
                return false;
        }
    }

    bool isHdr10Format(const VkFormat format) {
        switch (format) {
            case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
            case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
                return true;
            default:
                return false;
        }
    }
}

layer::SwapchainColorPipeline layer::classifySwapchainColor(
        const VkFormat format, const VkColorSpaceKHR colorSpace) {
    if (colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT) {
        if (isHdr10Format(format)) {
            return {
                .encoding = backend::FrameEncoding::Hdr10Pq,
                .exchangeFormat = VK_FORMAT_R16G16B16A16_SFLOAT,
                .generationSupported = true,
                .hdr = true,
                .name = "hdr10-pq",
            };
        }
        return {
            .generationSupported = false,
            .hdr = true,
            .name = "unsupported-hdr10-format",
            .reason = "HDR10/PQ requires an A2R10G10B10 or A2B10G10R10 swapchain",
        };
    }

    if (colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT) {
        if (format == VK_FORMAT_R16G16B16A16_SFLOAT) {
            return {
                .encoding = backend::FrameEncoding::ScRgbLinear,
                .exchangeFormat = VK_FORMAT_R16G16B16A16_SFLOAT,
                .generationSupported = true,
                .hdr = true,
                .name = "scrgb-linear",
            };
        }
        return {
            .generationSupported = false,
            .hdr = true,
            .name = "unsupported-scrgb-format",
            .reason = "linear scRGB requires an R16G16B16A16_SFLOAT swapchain",
        };
    }

    if (colorSpace == VK_COLOR_SPACE_HDR10_HLG_EXT ||
            colorSpace == VK_COLOR_SPACE_DOLBYVISION_EXT) {
        return {
            .generationSupported = false,
            .hdr = true,
            .name = "unsupported-hdr-colorspace",
            .reason = "the selected HDR transfer function is not supported",
        };
    }

    if (colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
        return {
            .generationSupported = false,
            .name = "unsupported-wide-color",
            .reason = "the swapchain uses an unvalidated non-sRGB colour space",
        };
    }

    if (isEightBitSdrFormat(format)) {
        return {
            .encoding = backend::FrameEncoding::Sdr8,
            .exchangeFormat = VK_FORMAT_R8G8B8A8_UNORM,
            .generationSupported = true,
            .name = "sdr-8-bit",
        };
    }

    if (isHighPrecisionSdrFormat(format)) {
        return {
            .encoding = backend::FrameEncoding::SdrHighPrecision,
            .exchangeFormat = VK_FORMAT_R16G16B16A16_SFLOAT,
            .generationSupported = true,
            .name = "sdr-high-precision",
        };
    }

    return {
        .generationSupported = false,
        .name = "unsupported-sdr-format",
        .reason = "the swapchain format has no validated LSFG transport",
    };
}
