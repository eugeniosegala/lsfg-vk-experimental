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
#include "adaptive_scheduler.hpp"

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
        AdaptivePresentationRecoveryPolicy presentationRecoveryPolicy;
        bool nextContextIsRecovery{false};
        size_t nextContextGenerationLimit{0};
        size_t nextContextLoadFallbackGenerationLimit{0};
        double nextContextLoadBaselineBaseFps{0.0};
        bool nextContextIsDiscontinuityRecovery{false};
        size_t nextContextDiscontinuityFallbackGenerationLimit{0};
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
            size_t recoveryLoadFallbackGenerationLimit,
            double recoveryLoadBaselineBaseFps,
            bool discontinuityRecoveryContext,
            size_t discontinuityFallbackGenerationLimit,
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
        /// stable identifier used to correlate this context's diagnostics
        [[nodiscard]] uint64_t diagnosticsId() const {
            return this->diagnosticsContextId;
        }
    private:
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
        std::optional<AdaptiveScheduler> adaptiveScheduler;
        AdaptiveRecoveryState* adaptiveRecoveryState{};
        uint64_t diagnosticsContextId{0};

        ls::GameConf profile;
        SwapchainInfo info;
    };

}
