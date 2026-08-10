/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include "lsfg-vk-backend/lsfgvk.hpp"
#include "lsfg-vk-common/configuration/config.hpp"
#include "lsfg-vk-common/helpers/pointers.hpp"
#include "lsfg-vk-common/vulkan/command_buffer.hpp"
#include "lsfg-vk-common/vulkan/fence.hpp"
#include "lsfg-vk-common/vulkan/image.hpp"
#include "lsfg-vk-common/vulkan/semaphore.hpp"
#include "lsfg-vk-common/vulkan/timeline_semaphore.hpp"
#include "lsfg-vk-common/vulkan/vulkan.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace lsfgvk::layer {

    /// swapchain info struct
    struct SwapchainInfo {
        std::vector<VkImage> images;
        VkFormat format;
        VkColorSpaceKHR colorSpace;
        VkExtent2D extent;
        VkPresentModeKHR presentMode;
    };

    /// modify the swapchain create info based on the profile pre-swapchain creation
    /// @param profile active game profile
    /// @param maxImages maximum number of images supported by the surface
    /// @param createInfo swapchain create info to modify
    void context_ModifySwapchainCreateInfo(const ls::GameConf& profile, uint32_t maxImages,
        VkSwapchainCreateInfoKHR& createInfo);

    /// Recovery coordination that survives a game-owned swapchain recreation.
    struct AdaptiveRecoveryState {
        std::optional<std::chrono::steady_clock::time_point> lastSwapchainRecreation;
        bool nextContextIsRecovery{false};
        size_t nextContextGenerationLimit{0};
        bool nextContextIsDiscontinuityRecovery{false};
        double nextContextDiscontinuityBaselineBaseFps{0.0};
        std::optional<std::chrono::steady_clock::time_point>
            nextContextDiscontinuityDeadline;
        bool nextContextDiscontinuitySoftRecoveryAttempted{false};
    };

    /// swapchain context for a layer instance
    class Swapchain {
    public:
        /// create a new swapchain context
        /// @param vk vulkan instance
        /// @param backend lsfg-vk backend instance
        /// @param profile active game profile
        /// @param info swapchain info
        /// @param recoveryState recovery coordination shared across swapchains
        /// @param recoveryContext true when this context follows guarded recovery
        Swapchain(const vk::Vulkan& vk, backend::Instance& backend,
            ls::GameConf profile, SwapchainInfo info,
            AdaptiveRecoveryState* recoveryState, bool recoveryContext,
            size_t recoveryGenerationLimit,
            bool discontinuityRecoveryContext,
            double discontinuityBaselineBaseFps,
            std::optional<std::chrono::steady_clock::time_point>
                discontinuityDeadline,
            bool discontinuitySoftRecoveryAttempted);

        /// present a frame
        /// @param vk vulkan instance
        /// @param queue presentation queue
        /// @param next_chain next chain pointer for the present info (WARN: shared!)
        /// @param imageIdx swapchain image index to present to
        /// @param semaphores semaphores to wait on before presenting
        /// @throws ls::vulkan_error on vulkan errors
        VkResult present(const vk::Vulkan& vk,
            VkQueue queue, VkSwapchainKHR swapchain,
            void* next_chain, uint32_t imageIdx,
            const std::vector<VkSemaphore>& semaphores);
    private:
        /// calculate generated-frame timestamps for the current real frame
        std::vector<float> generatedFrameTimestamps(
            std::chrono::steady_clock::time_point now
        );
        /// reset timing state after a compositor presentation discontinuity
        void resetAdaptiveScheduler(
            std::chrono::steady_clock::time_point now
        );
        /// run real frames only while game/compositor cadence settles
        void beginAdaptiveStabilization(
            std::chrono::steady_clock::time_point now,
            std::string_view reason
        );
        /// rearm an interrupted or rejected probe once its recovery policy allows
        void scheduleAdaptiveRearm(
            std::chrono::steady_clock::time_point now,
            std::string_view reason,
            size_t fallbackLimit = 0,
            double baselineBaseFps = 0.0
        );
        /// return the generation level that was proven before a transient probe
        size_t validatedAdaptiveGenerationLimit() const;
        /// retain a proven level after recovery while delaying higher probes
        void restoreAdaptiveGenerationLimit(
            std::chrono::steady_clock::time_point now,
            size_t generationLimit,
            std::string_view reason
        );
        /// retain a proven pre-overlay cadence until real presentation settles
        void beginAdaptiveDiscontinuityRecovery(
            std::chrono::steady_clock::time_point now,
            size_t generationLimit,
            double baselineBaseFps,
            std::optional<std::chrono::steady_clock::time_point> deadline,
            bool softRecoveryAttempted,
            std::string_view reason
        );
        /// ramp generated-frame load and reject counterproductive steps
        void updateAdaptiveGenerationLimit(
            std::chrono::steady_clock::time_point now,
            double baseFps
        );

        std::vector<vk::Image> sourceImages;
        std::vector<vk::Image> destinationImages;
        ls::lazy<vk::TimelineSemaphore> syncSemaphore;

        ls::lazy<vk::CommandBuffer> renderCommandBuffer;
        ls::lazy<vk::Fence> renderFence;
        struct RenderPass {
            vk::CommandBuffer commandBuffer;
            vk::Semaphore acquireSemaphore;
        };
        std::vector<RenderPass> passes;
        std::vector<std::pair<vk::Semaphore, vk::Semaphore>> postCopySemaphores;

        ls::R<backend::Instance> instance;
        ls::owned_ptr<ls::R<backend::Context>> ctx;
        size_t idx{1};
        size_t fidx{0}; // real frame index
        bool generatedImageAcquireBackoff{false};
        size_t generatedImageAcquireBypassCount{0};
        std::optional<std::chrono::steady_clock::time_point> generatedImageAcquireLastBoundedProbe;
        bool swapchainRecreationRequested{false};
        size_t adaptiveHistoryWarmupRemaining{0};
        bool adaptiveHistoryWarmupIsRecovery{false};

        std::optional<std::chrono::steady_clock::time_point> adaptiveLastRealFrame;
        std::optional<std::chrono::steady_clock::time_point> adaptiveLastDiagnostic;
        double adaptiveSmoothedIntervalSeconds{0.0};
        double adaptiveOutputCredit{0.0};
        std::optional<std::chrono::steady_clock::time_point> adaptiveStabilizationUntil;
        std::optional<std::chrono::steady_clock::time_point> adaptiveNextRampAt;
        std::optional<std::chrono::steady_clock::time_point> adaptiveRampEvaluationAt;
        size_t adaptiveGenerationLimit{0};
        size_t adaptiveRampPreviousLimit{0};
        size_t adaptiveCadenceDropFrames{0};
        double adaptiveRampBaselineBaseFps{0.0};
        bool adaptiveBridgeActive{false};
        size_t adaptiveBridgeBaselineLimit{0};
        double adaptiveBridgeBaselineBaseFps{0.0};
        bool adaptiveRearmRequired{false};
        std::optional<std::chrono::steady_clock::time_point> adaptiveRearmNotBefore;
        std::optional<std::chrono::steady_clock::time_point> adaptiveStableRearmSince;
        std::optional<std::chrono::steady_clock::time_point> adaptiveRearmImprovementSince;
        std::string adaptiveRearmReason;
        double adaptiveRearmBaselineBaseFps{0.0};
        // The last validated generation level to retain while a higher adaptive
        // probe settles after interruption or cools down after rejection.
        size_t adaptiveRearmFallbackLimit{0};
        // A bounded preference for a constant generated-frame cadence. This is
        // used only when the strict target scheduler would otherwise alternate
        // between frame counts (for example, 60 real FPS toward a 90 FPS target).
        std::optional<size_t> adaptiveStableCadenceLimit;
        std::optional<std::chrono::steady_clock::time_point> adaptiveStableCadenceEvaluationAt;
        std::optional<std::chrono::steady_clock::time_point> adaptiveStableCadenceOutsideRangeSince;
        std::optional<std::chrono::steady_clock::time_point> adaptiveStableCadenceRetryAt;
        double adaptiveStableCadenceBaselineBaseFps{0.0};
        std::optional<std::chrono::steady_clock::time_point> adaptiveRescueUntil;
        std::optional<std::chrono::steady_clock::time_point> adaptiveRescueCooldownUntil;
        size_t adaptiveRescuePreviousLimit{0};
        double adaptiveRescueBaselineBaseFps{0.0};
        bool adaptiveRescueFromStrictLoad{false};
        size_t adaptiveRescueStrictLoadLimit{0};
        size_t adaptiveStrictLoadBaselineLimit{0};
        double adaptiveStrictLoadBaselineBaseFps{0.0};
        std::optional<std::chrono::steady_clock::time_point>
            adaptiveStrictLoadCollapseSince;
        std::optional<std::chrono::steady_clock::time_point>
            adaptiveDiscontinuityRecoveryDeadline;
        std::optional<std::chrono::steady_clock::time_point>
            adaptiveDiscontinuityStableSince;
        size_t adaptiveDiscontinuityGenerationLimit{0};
        double adaptiveDiscontinuityBaselineBaseFps{0.0};
        bool adaptiveDiscontinuitySoftRecoveryAttempted{false};
        size_t adaptiveConsecutiveProbeFailures{0};
        size_t adaptiveLastFailedRampLimit{0};
        size_t adaptiveConsecutiveRampFailures{0};
        double adaptiveFailedRampBaselineBaseFps{0.0};
        AdaptiveRecoveryState* adaptiveRecoveryState{};

        ls::GameConf profile;
        SwapchainInfo info;
    };

}
