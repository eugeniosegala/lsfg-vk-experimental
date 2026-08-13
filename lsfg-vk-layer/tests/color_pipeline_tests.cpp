/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "color_pipeline.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

using namespace lsfgvk;

namespace {
    void expect(const bool condition, const std::string_view message) {
        if (condition)
            return;
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

int main() {
    const auto sdr = layer::classifySwapchainColor(
        VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR
    );
    expect(sdr.generationSupported, "8-bit sRGB should be supported");
    expect(sdr.encoding == backend::FrameEncoding::Sdr8,
        "8-bit sRGB should select the SDR transport");
    expect(!sdr.hdr, "8-bit sRGB should not be HDR");

    const auto sdr10 = layer::classifySwapchainColor(
        VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR
    );
    expect(sdr10.generationSupported, "10-bit SDR should remain supported");
    expect(sdr10.encoding == backend::FrameEncoding::SdrHighPrecision,
        "10-bit SDR must not activate HDR model semantics");
    expect(!sdr10.hdr, "10-bit SDR should not be classified as HDR");

    for (const auto format : {
            VK_FORMAT_A2B10G10R10_UNORM_PACK32,
            VK_FORMAT_A2R10G10B10_UNORM_PACK32}) {
        const auto hdr10 = layer::classifySwapchainColor(
            format, VK_COLOR_SPACE_HDR10_ST2084_EXT
        );
        expect(hdr10.generationSupported, "Gamescope HDR10 formats should be supported");
        expect(hdr10.encoding == backend::FrameEncoding::Hdr10Pq,
            "HDR10 should select PQ-to-scRGB conversion");
        expect(hdr10.hdr, "HDR10 should be classified as HDR");
    }

    const auto scrgb = layer::classifySwapchainColor(
        VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT
    );
    expect(scrgb.generationSupported, "Gamescope scRGB should be supported");
    expect(scrgb.encoding == backend::FrameEncoding::ScRgbLinear,
        "scRGB should use the native linear HDR model path");

    const auto badScrgb = layer::classifySwapchainColor(
        VK_FORMAT_A2B10G10R10_UNORM_PACK32,
        VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT
    );
    expect(!badScrgb.generationSupported,
        "invalid packed scRGB combinations should fail safely");

    const auto hlg = layer::classifySwapchainColor(
        VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_HLG_EXT
    );
    expect(!hlg.generationSupported, "HLG should pass through until implemented");
    expect(hlg.hdr, "unsupported HLG should still be diagnosed as HDR");

    std::cout << "All swapchain colour-pipeline tests passed.\n";
    return 0;
}
