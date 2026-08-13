/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include "lsfg-vk-common/configuration/config.hpp"

#include <cstddef>
#include <cstdint>

namespace lsfgvk::layer {

    enum class ProfileUpdateAction : uint8_t {
        NoRuntimeChange,
        ApplyLive,
        DeferUntilSwapchainRecreation,
    };

    struct ProfileUpdateDecision {
        ProfileUpdateAction action{ProfileUpdateAction::NoRuntimeChange};
        bool frameGenerationChanged{false};
        bool adaptivePolicyChanged{false};
    };

    /// Classify a profile change without touching Vulkan state.
    ///
    /// Only switches that alter CPU-side policy or select the already-created
    /// frame-generation resources are safe during vkQueuePresentKHR. Changes
    /// that alter resource shape or backend model construction are retained by
    /// Root for the next game-owned swapchain creation.
    [[nodiscard]] inline ProfileUpdateDecision classifyProfileUpdate(
            const ls::GameConf& current, const ls::GameConf& next,
            const size_t generatedFrameCapacity,
            const bool frameGenerationResourcesAvailable) {
        const bool frameGenerationChanged =
            current.frame_generation_enabled != next.frame_generation_enabled;
        const bool adaptivePolicyChanged = current.adaptive && next.adaptive && (
            current.target_fps != next.target_fps ||
            current.adaptive_max_multiplier != next.adaptive_max_multiplier ||
            current.adaptive_stable_cadence != next.adaptive_stable_cadence
        );

        const bool backendConstructionChanged =
            current.gpu != next.gpu ||
            current.flow_scale != next.flow_scale ||
            current.performance_mode != next.performance_mode;
        const bool presentationShapeChanged =
            current.multiplier != next.multiplier ||
            current.adaptive != next.adaptive ||
            current.pacing != next.pacing;
        const bool adaptiveCapacityExceeded = next.adaptive &&
            next.adaptive_max_multiplier - 1 > generatedFrameCapacity;
        const bool resourcesNeeded = !current.frame_generation_enabled &&
            next.frame_generation_enabled &&
            !frameGenerationResourcesAvailable;

        if (backendConstructionChanged || presentationShapeChanged ||
                adaptiveCapacityExceeded || resourcesNeeded) {
            return {
                .action = ProfileUpdateAction::DeferUntilSwapchainRecreation,
                .frameGenerationChanged = frameGenerationChanged,
                .adaptivePolicyChanged = adaptivePolicyChanged,
            };
        }

        if (frameGenerationChanged || adaptivePolicyChanged) {
            return {
                .action = ProfileUpdateAction::ApplyLive,
                .frameGenerationChanged = frameGenerationChanged,
                .adaptivePolicyChanged = adaptivePolicyChanged,
            };
        }

        return {};
    }

}
