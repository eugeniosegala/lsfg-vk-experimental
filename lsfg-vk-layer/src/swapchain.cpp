/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "swapchain.hpp"
#include "lsfg-vk-backend/lsfgvk.hpp"
#include "lsfg-vk-common/configuration/config.hpp"
#include "lsfg-vk-common/helpers/errors.hpp"
#include "lsfg-vk-common/helpers/pointers.hpp"
#include "lsfg-vk-common/vulkan/command_buffer.hpp"
#include "lsfg-vk-common/vulkan/image.hpp"
#include "lsfg-vk-common/vulkan/semaphore.hpp"
#include "lsfg-vk-common/vulkan/vulkan.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

using namespace lsfgvk;
using namespace lsfgvk::layer;

namespace {
    using DiagnosticsClock = std::chrono::steady_clock;

    bool presentDiagnosticsEnabled() {
        static const bool enabled = [] {
            const char* value = std::getenv("LSFGVK_PRESENT_DIAGNOSTICS");
            return value && std::string_view(value) != "0";
        }();
        return enabled;
    }

    double presentDiagnosticsThresholdMs() {
        static const double threshold = [] {
            constexpr double defaultThresholdMs = 20.0;
            const char* value = std::getenv("LSFGVK_PRESENT_DIAGNOSTICS_THRESHOLD_MS");
            if (!value)
                return defaultThresholdMs;

            char* end{};
            const double parsed = std::strtod(value, &end);
            if (end == value || *end != '\0' || parsed < 0.0)
                return defaultThresholdMs;
            return parsed;
        }();
        return threshold;
    }

    std::optional<uint64_t> generatedImageAcquireTimeoutNs() {
        static const std::optional<uint64_t> timeout = []() -> std::optional<uint64_t> {
            const char* value = std::getenv("LSFGVK_PRESENT_ACQUIRE_TIMEOUT_MS");
            if (!value)
                return std::nullopt;

            char* end{};
            const double parsed = std::strtod(value, &end);
            if (end == value || *end != '\0' || parsed <= 0.0)
                return std::nullopt;

            constexpr uint64_t nanosecondsPerMillisecond = 1'000'000;
            const double maximumMilliseconds = static_cast<double>(
                (std::numeric_limits<uint64_t>::max() - 1) / nanosecondsPerMillisecond
            );
            const double clampedMilliseconds = std::min(parsed, maximumMilliseconds);
            return static_cast<uint64_t>(
                clampedMilliseconds * static_cast<double>(nanosecondsPerMillisecond)
            );
        }();
        return timeout;
    }

    double elapsedMilliseconds(const DiagnosticsClock::time_point start) {
        return std::chrono::duration<double, std::milli>(
            DiagnosticsClock::now() - start
        ).count();
    }

    DiagnosticsClock::time_point startPresentDiagnostic() {
        if (!presentDiagnosticsEnabled())
            return {};
        return DiagnosticsClock::now();
    }

    void logSlowPresentOperation(std::string_view operation,
            size_t frameIndex, size_t sequenceIndex,
            const DiagnosticsClock::time_point start,
            std::optional<VkResult> result = std::nullopt,
            std::optional<size_t> passIndex = std::nullopt,
            std::optional<uint32_t> imageIndex = std::nullopt) {
        if (!presentDiagnosticsEnabled())
            return;

        const double durationMs = elapsedMilliseconds(start);
        const bool resultFailed = result &&
            *result != VK_SUCCESS && *result != VK_SUBOPTIMAL_KHR;
        if (durationMs < presentDiagnosticsThresholdMs() && !resultFailed)
            return;

        std::ostringstream message;
        message << "lsfg-vk: present diagnostics: operation=" << operation
                << " duration_ms=" << durationMs
                << " frame=" << frameIndex
                << " sequence=" << sequenceIndex;
        if (result)
            message << " result=" << static_cast<int>(*result);
        if (passIndex)
            message << " pass=" << *passIndex;
        if (imageIndex)
            message << " image=" << *imageIndex;
        std::cerr << message.str() << '\n';
    }

    void logPresentFallback(size_t frameIndex, size_t sequenceIndex,
            size_t passIndex, size_t skippedFrames, uint64_t timelineValue) {
        if (!presentDiagnosticsEnabled())
            return;

        std::cerr << "lsfg-vk: present diagnostics: operation=skip-generated-frames"
                  << " frame=" << frameIndex
                  << " sequence=" << sequenceIndex
                  << " pass=" << passIndex
                  << " skipped=" << skippedFrames
                  << " wait_timeline=" << timelineValue << '\n';
    }

    VkImageMemoryBarrier barrierHelper(VkImage handle,
            VkAccessFlags srcAccessMask,
            VkAccessFlags dstAccessMask,
            VkImageLayout oldLayout,
            VkImageLayout newLayout) {
        return VkImageMemoryBarrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = srcAccessMask,
            .dstAccessMask = dstAccessMask,
            .oldLayout = oldLayout,
            .newLayout = newLayout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = handle,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1
            }
        };
    }
}

void layer::context_ModifySwapchainCreateInfo(const ls::GameConf& profile, uint32_t maxImages,
        VkSwapchainCreateInfoKHR& createInfo) {
    createInfo.imageUsage |=
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    switch (profile.pacing) {
        case ls::Pacing::None:
            createInfo.minImageCount += profile.multiplier;
            if (maxImages && createInfo.minImageCount > maxImages)
                createInfo.minImageCount = maxImages;

            createInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
            break;
    }
}

Swapchain::Swapchain(const vk::Vulkan& vk, backend::Instance& backend,
            ls::GameConf profile, SwapchainInfo info) :
        instance(backend),
        profile(std::move(profile)), info(std::move(info)) {
    const VkExtent2D extent = this->info.extent;
    const bool hdr = this->info.format > 57;

    std::vector<int> sourceFds(2);
    std::vector<int> destinationFds(this->profile.multiplier - 1);

    this->sourceImages.reserve(sourceFds.size());
    for (int& fd : sourceFds)
        this->sourceImages.emplace_back(vk,
            extent, hdr ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            std::nullopt, &fd);

    this->destinationImages.reserve(destinationFds.size());
    for (int& fd : destinationFds)
        this->destinationImages.emplace_back(vk,
            extent, hdr ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            std::nullopt, &fd);

    int syncFd{};
    this->syncSemaphore.emplace(vk, 0, std::nullopt, &syncFd);

    try {
        this->ctx = ls::owned_ptr<ls::R<backend::Context>>(
            new ls::R<backend::Context>(backend.openContext(
                { sourceFds.at(0), sourceFds.at(1) }, destinationFds, syncFd,
                extent.width, extent.height,
                hdr, 1.0F / this->profile.flow_scale, this->profile.performance_mode
            )),
            [backend = &backend](ls::R<backend::Context>& ctx) {
                backend->closeContext(ctx);
            }
        );

        backend::makeLeaking(); // don't worry about it :3
    } catch (const std::exception& e) {
        throw ls::error("failed to create swapchain context", e);
    }

    this->renderCommandBuffer.emplace(vk);
    this->renderFence.emplace(vk);
    for (size_t i = 0; i < this->destinationImages.size(); i++) {
        this->passes.emplace_back(RenderPass {
            .commandBuffer = vk::CommandBuffer(vk),
            .acquireSemaphore = vk::Semaphore(vk)
        });
    }

    const size_t frames = std::max(this->info.images.size(), this->destinationImages.size() + 2);
    for (size_t i = 0; i < frames; i++) {
        this->postCopySemaphores.emplace_back(
            vk::Semaphore(vk),
            vk::Semaphore(vk)
        );
    }

    if (presentDiagnosticsEnabled()) {
        std::cerr << "lsfg-vk: present diagnostics enabled; slow operation threshold is "
                  << presentDiagnosticsThresholdMs() << " ms\n";
    }
    if (const auto timeout = generatedImageAcquireTimeoutNs()) {
        std::cerr << "lsfg-vk: generated-image acquire timeout enabled at "
                  << static_cast<double>(*timeout) / 1'000'000.0
                  << " ms; stalled generated frames will be skipped\n";
    }
}

VkResult Swapchain::present(const vk::Vulkan& vk,
        VkQueue queue, VkSwapchainKHR swapchain,
        void* next_chain, uint32_t imageIdx,
        const std::vector<VkSemaphore>& semaphores) {
    const auto presentStarted = startPresentDiagnostic();
    const auto& swapchainImage = this->info.images.at(imageIdx);
    const auto& sourceImage = this->sourceImages.at(this->fidx % 2);

    // schedule frame generation
    const auto scheduleStarted = startPresentDiagnostic();
    try {
        this->instance.get().scheduleFrames(this->ctx.get());
    } catch (const std::exception& e) {
        throw ls::error("failed to schedule frames", e);
    }
    logSlowPresentOperation("schedule-frames", this->fidx, this->idx, scheduleStarted);

    // update present mode when not using pacing
    if (this->profile.pacing == ls::Pacing::None) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-warning-option"
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
        auto* info = reinterpret_cast<VkSwapchainPresentModeInfoEXT*>(next_chain);
        while (info) {
            if (info->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT) {
                for (size_t i = 0; i < info->swapchainCount; i++)
                    const_cast<VkPresentModeKHR*>(info->pPresentModes)[i] =
                        VK_PRESENT_MODE_FIFO_KHR;
            }

            info = reinterpret_cast<VkSwapchainPresentModeInfoEXT*>(const_cast<void*>(info->pNext));
        }
#pragma clang diagnostic pop
    }

    // wait for completion of previous frame
    if (this->fidx) {
        const auto fenceWaitStarted = startPresentDiagnostic();
        const bool fenceSignaled = this->renderFence->wait(vk, 150ULL * 1000 * 1000);
        logSlowPresentOperation(
            "wait-render-fence", this->fidx, this->idx, fenceWaitStarted,
            fenceSignaled ? VK_SUCCESS : VK_TIMEOUT
        );
        if (!fenceSignaled)
            throw ls::vulkan_error(VK_TIMEOUT, "vkWaitForFences() failed");
    }
    this->renderFence->reset(vk);

    // copy swapchain image into backend source image
    const auto& cmdbuf = *this->renderCommandBuffer;
    cmdbuf.begin(vk);

    cmdbuf.blitImage(vk,
        {
            barrierHelper(swapchainImage,
                VK_ACCESS_NONE,
                VK_ACCESS_TRANSFER_READ_BIT,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
            ),
            barrierHelper(sourceImage.handle(),
                VK_ACCESS_NONE,
                VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
            ),
        },
        { swapchainImage, sourceImage.handle() },
        sourceImage.getExtent(),
        {
            barrierHelper(swapchainImage,
                VK_ACCESS_TRANSFER_READ_BIT,
                VK_ACCESS_MEMORY_READ_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
            ),
        }
    );

    cmdbuf.end(vk);
    const auto sourceSubmitStarted = startPresentDiagnostic();
    cmdbuf.submit(vk,
        semaphores, VK_NULL_HANDLE, 0,
        {}, this->syncSemaphore->handle(), this->idx++
    );
    logSlowPresentOperation("submit-source-copy", this->fidx, this->idx, sourceSubmitStarted);

    const auto presentOriginalImage = [&](VkSemaphore waitSemaphore, void* presentNextChain) {
        const VkPresentInfoKHR presentInfo{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .pNext = presentNextChain,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &waitSemaphore,
            .swapchainCount = 1,
            .pSwapchains = &swapchain,
            .pImageIndices = &imageIdx,
        };
        const auto originalPresentStarted = startPresentDiagnostic();
        const auto result = vk.df().QueuePresentKHR(queue, &presentInfo);
        logSlowPresentOperation(
            "present-original-image", this->fidx, this->idx, originalPresentStarted, result,
            std::nullopt, imageIdx
        );
        return result;
    };

    for (size_t i = 0; i < this->destinationImages.size(); i++) {
        auto& pcs = this->postCopySemaphores.at(this->idx % this->postCopySemaphores.size());
        auto& destinationImage = this->destinationImages.at(i);
        auto& pass = this->passes.at(i);

        // acquire swapchain image
        uint32_t aqImageIdx{};
        const auto acquireStarted = startPresentDiagnostic();
        const auto acquireTimeout = generatedImageAcquireTimeoutNs();
        auto res = vk.df().AcquireNextImageKHR(vk.dev(), swapchain,
            acquireTimeout.value_or(UINT64_MAX), pass.acquireSemaphore.handle(),
            VK_NULL_HANDLE,
            &aqImageIdx
        );
        logSlowPresentOperation(
            "acquire-generated-image", this->fidx, this->idx, acquireStarted, res,
            i, aqImageIdx
        );
        if (acquireTimeout && (res == VK_TIMEOUT || res == VK_NOT_READY)) {
            // Gamescope can temporarily stop releasing the extra swapchain images used for generated frames while
            // an overlay is visible. Do not block the game indefinitely. The backend has already scheduled every
            // generated frame for this sequence, so wait for its final timeline value before presenting the original
            // image and advancing both sides to the next sequence.
            const size_t skippedFrames = this->destinationImages.size() - i;
            const uint64_t finalGeneratedTimelineValue = this->idx + skippedFrames - 1;
            auto& fallbackSemaphore = pcs.second;

            auto& fallbackCommandBuffer = pass.commandBuffer;
            fallbackCommandBuffer.begin(vk);
            fallbackCommandBuffer.end(vk);
            fallbackCommandBuffer.submit(vk,
                {}, this->syncSemaphore->handle(), finalGeneratedTimelineValue,
                { fallbackSemaphore.handle() }, VK_NULL_HANDLE, 0,
                this->renderFence->handle()
            );

            logPresentFallback(
                this->fidx, this->idx, i, skippedFrames, finalGeneratedTimelineValue
            );
            this->idx += skippedFrames;

            res = presentOriginalImage(
                fallbackSemaphore.handle(), i == 0 ? next_chain : nullptr
            );
            if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
                throw ls::vulkan_error(res, "vkQueuePresentKHR() failed");

            logSlowPresentOperation("present-total", this->fidx, this->idx, presentStarted, res);
            this->fidx++;
            return res;
        }
        if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
            throw ls::vulkan_error(res, "vkAcquireNextImageKHR() failed");

        const auto& aquiredSwapchainImage = this->info.images.at(aqImageIdx);

        // copy backend destination image into swapchain image
        auto& cmdbuf = pass.commandBuffer;
        cmdbuf.begin(vk);

        cmdbuf.blitImage(vk,
            {
                barrierHelper(destinationImage.handle(),
                    VK_ACCESS_NONE,
                    VK_ACCESS_TRANSFER_READ_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                ),
                barrierHelper(aquiredSwapchainImage,
                    VK_ACCESS_NONE,
                    VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                ),
            },
            { destinationImage.handle(), aquiredSwapchainImage },
            destinationImage.getExtent(),
            {
                barrierHelper(aquiredSwapchainImage,
                    VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_MEMORY_READ_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                ),
            }
        );

        std::vector<VkSemaphore> waitSemaphores{ pass.acquireSemaphore.handle() };
        if (i) { // non-first pass
            const auto& prevPCS = this->postCopySemaphores.at((this->idx - 1) % this->postCopySemaphores.size());
            waitSemaphores.push_back(prevPCS.second.handle());
        }

        const std::vector<VkSemaphore> signalSemaphores{
            pcs.first.handle(),
            pcs.second.handle()
        };

        cmdbuf.end(vk);
        const auto generatedSubmitStarted = startPresentDiagnostic();
        cmdbuf.submit(vk,
            waitSemaphores, this->syncSemaphore->handle(), this->idx,
            signalSemaphores, VK_NULL_HANDLE, 0,
            i == this->destinationImages.size() - 1 ? this->renderFence->handle() : VK_NULL_HANDLE
        );
        logSlowPresentOperation(
            "submit-generated-copy", this->fidx, this->idx, generatedSubmitStarted,
            std::nullopt, i
        );

        // present swapchain image
        const VkPresentInfoKHR presentInfo{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .pNext = i ? nullptr : next_chain,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &pcs.first.handle(),
            .swapchainCount = 1,
            .pSwapchains = &swapchain,
            .pImageIndices = &aqImageIdx,
        };
        const auto generatedPresentStarted = startPresentDiagnostic();
        res = vk.df().QueuePresentKHR(queue,
            &presentInfo);
        logSlowPresentOperation(
            "present-generated-image", this->fidx, this->idx, generatedPresentStarted, res,
            i, aqImageIdx
        );
        if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
            throw ls::vulkan_error(res, "vkQueuePresentKHR() failed");

        this->idx++;
    }

    // present original swapchain image
    auto& lastPCS = this->postCopySemaphores.at((this->idx - 1) % this->postCopySemaphores.size());
    auto res = presentOriginalImage(lastPCS.second.handle(), nullptr);
    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
        throw ls::vulkan_error(res, "vkQueuePresentKHR() failed");

    logSlowPresentOperation("present-total", this->fidx, this->idx, presentStarted, res);
    this->fidx++;
    return res;
}
