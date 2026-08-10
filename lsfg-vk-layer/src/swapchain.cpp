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
#include <cmath>
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

    constexpr size_t adaptiveMaximumMultiplier = 4;
    constexpr size_t adaptiveHistoryWarmupFrames = 3;
    constexpr double adaptiveMinimumBaseFps = 10.0;
    constexpr double adaptiveIntervalSmoothing = 0.25;

    size_t generatedFrameCapacity(const ls::GameConf& profile) {
        const size_t multiplier = profile.adaptive
            ? adaptiveMaximumMultiplier
            : profile.multiplier;
        return multiplier - 1;
    }

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

    constexpr auto generatedImageAcquireBoundedProbeInterval = std::chrono::seconds(1);

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
            size_t passIndex, size_t skippedFrames, uint64_t timelineValue,
            std::string_view acquireMode, std::string_view backendWork) {
        if (!presentDiagnosticsEnabled())
            return;

        std::cerr << "lsfg-vk: present diagnostics: operation=skip-generated-frames"
                  << " frame=" << frameIndex
                  << " sequence=" << sequenceIndex
                  << " pass=" << passIndex
                  << " skipped=" << skippedFrames
                  << " wait_timeline=" << timelineValue
                  << " acquire_mode=" << acquireMode
                  << " backend_work=" << backendWork
                  << '\n';
    }

    void logPresentRecovery(size_t frameIndex, size_t sequenceIndex,
            size_t passIndex, uint32_t imageIndex, size_t bypassedFrames,
            std::string_view acquireMode, size_t warmupFrames) {
        if (!presentDiagnosticsEnabled())
            return;

        std::cerr << "lsfg-vk: present diagnostics: operation="
                  << (warmupFrames ? "generated-image-recovered" : "resume-generated-frames")
                  << " frame=" << frameIndex
                  << " sequence=" << sequenceIndex
                  << " pass=" << passIndex
                  << " image=" << imageIndex
                  << " acquire_mode=" << acquireMode
                  << " bypassed_frames=" << bypassedFrames;
        if (warmupFrames)
            std::cerr << " recovery_warmup_frames=" << warmupFrames;
        std::cerr << '\n';
    }

    void logHistoryWarmup(size_t frameIndex, size_t sequenceIndex,
            size_t remainingFrames, bool recovery,
            std::optional<uint32_t> acquiredImage) {
        if (!presentDiagnosticsEnabled())
            return;

        std::cerr << "lsfg-vk: present diagnostics: operation=history-warmup"
                  << " frame=" << frameIndex
                  << " sequence=" << sequenceIndex
                  << " reason=" << (recovery ? "recovery" : "startup")
                  << " remaining_frames=" << remainingFrames;
        if (acquiredImage)
            std::cerr << " released_image=" << *acquiredImage;
        std::cerr << '\n';
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
            createInfo.minImageCount += generatedFrameCapacity(profile) + 1;
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
    if (this->profile.adaptive)
        this->adaptiveHistoryWarmupRemaining = adaptiveHistoryWarmupFrames;

    const VkExtent2D extent = this->info.extent;
    const bool hdr = this->info.format > 57;

    std::vector<int> sourceFds(2);
    std::vector<int> destinationFds(generatedFrameCapacity(this->profile));

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
    if (this->profile.adaptive) {
        std::cerr << "lsfg-vk: adaptive frame generation enabled; target="
                  << this->profile.target_fps
                  << " fps, maximum multiplier=" << adaptiveMaximumMultiplier << "x\n";
    }
}

std::vector<float> Swapchain::generatedFrameTimestamps(
        const std::chrono::steady_clock::time_point now) {
    if (!this->adaptiveLastRealFrame) {
        this->adaptiveLastRealFrame = now;
        return {};
    }

    const double rawIntervalSeconds = std::chrono::duration<double>(
        now - *this->adaptiveLastRealFrame
    ).count();
    this->adaptiveLastRealFrame = now;

    // Loading screens, suspension and base rates below 10 FPS do not provide
    // useful motion history. Match the Windows implementation's safety floor
    // by presenting the real frame without interpolation and restarting the
    // fractional accumulator after the stall.
    if (rawIntervalSeconds <= 0.0 ||
            rawIntervalSeconds > 1.0 / adaptiveMinimumBaseFps) {
        this->adaptiveSmoothedIntervalSeconds = 0.0;
        this->adaptiveOutputCredit = 0.0;
        return {};
    }

    if (this->adaptiveSmoothedIntervalSeconds == 0.0) {
        this->adaptiveSmoothedIntervalSeconds = rawIntervalSeconds;
    } else {
        this->adaptiveSmoothedIntervalSeconds =
            (1.0 - adaptiveIntervalSmoothing) * this->adaptiveSmoothedIntervalSeconds +
            adaptiveIntervalSmoothing * rawIntervalSeconds;
    }

    const double desiredOutputsPerRealFrame =
        this->adaptiveSmoothedIntervalSeconds *
        static_cast<double>(this->profile.target_fps);

    size_t generatedFrameCount = 0;
    if (desiredOutputsPerRealFrame > 1.0) {
        this->adaptiveOutputCredit += desiredOutputsPerRealFrame;
        const size_t requestedOutputs = std::max<size_t>(
            1,
            static_cast<size_t>(std::floor(this->adaptiveOutputCredit + 1e-9))
        );
        generatedFrameCount = std::min(
            requestedOutputs - 1,
            this->destinationImages.size()
        );
        this->adaptiveOutputCredit -= static_cast<double>(generatedFrameCount + 1);
        if (this->adaptiveOutputCredit < 0.0)
            this->adaptiveOutputCredit = 0.0;
        if (generatedFrameCount == this->destinationImages.size() &&
                this->adaptiveOutputCredit >= 1.0) {
            // The requested target is currently above the 4x ceiling. Keep
            // only the fractional phase instead of accumulating an impossible
            // backlog that would delay adaptation when the base rate recovers.
            this->adaptiveOutputCredit = std::fmod(this->adaptiveOutputCredit, 1.0);
        }
    } else {
        // A Vulkan layer cannot present fewer real frames than the application
        // submits. Do not carry debt when the base rate is already at or above
        // the requested target.
        this->adaptiveOutputCredit = 0.0;
    }

    if (presentDiagnosticsEnabled() &&
            (!this->adaptiveLastDiagnostic ||
             now - *this->adaptiveLastDiagnostic >= std::chrono::seconds(1))) {
        this->adaptiveLastDiagnostic = now;
        const double baseFps = 1.0 / this->adaptiveSmoothedIntervalSeconds;
        std::cerr << "lsfg-vk: present diagnostics: operation=adaptive-plan"
                  << " base_fps=" << baseFps
                  << " target_fps=" << this->profile.target_fps
                  << " generated=" << generatedFrameCount
                  << " max_generated=" << this->destinationImages.size()
                  << '\n';
    }

    std::vector<float> timestamps;
    timestamps.reserve(generatedFrameCount);
    for (size_t i = 0; i < generatedFrameCount; ++i)
        timestamps.push_back(
            static_cast<float>(i + 1) /
            static_cast<float>(generatedFrameCount + 1)
        );
    return timestamps;
}

void Swapchain::resetAdaptiveScheduler(
        const std::chrono::steady_clock::time_point now) {
    if (!this->profile.adaptive)
        return;

    this->adaptiveLastRealFrame = now;
    this->adaptiveSmoothedIntervalSeconds = 0.0;
    this->adaptiveOutputCredit = 0.0;
}

VkResult Swapchain::present(const vk::Vulkan& vk,
        VkQueue queue, VkSwapchainKHR swapchain,
        void* next_chain, uint32_t imageIdx,
        const std::vector<VkSemaphore>& semaphores) {
    const auto presentStarted = startPresentDiagnostic();
    const auto& swapchainImage = this->info.images.at(imageIdx);
    const auto& sourceImage = this->sourceImages.at(this->fidx % 2);
    const bool historyWarmupActive = this->adaptiveHistoryWarmupRemaining > 0;
    const auto generatedTimestamps = this->profile.adaptive && !historyWarmupActive
        ? this->generatedFrameTimestamps(DiagnosticsClock::now())
        : std::vector<float>{};
    const size_t generatedFrameCount = this->profile.adaptive
        ? generatedTimestamps.size()
        : this->destinationImages.size();

    const auto configuredAcquireTimeout = generatedImageAcquireTimeoutNs();
    bool renderFencePrepared = false;
    bool bypassGeneratedFrames = historyWarmupActive ||
        (this->profile.adaptive && generatedTimestamps.empty());
    bool generatedImageUnavailable = false;
    bool boundedRecoveryProbe = false;
    std::optional<uint32_t> preacquiredGeneratedImage;
    std::optional<uint32_t> recoveryWarmupImage;

    const auto prepareRenderFence = [&]() {
        if (renderFencePrepared)
            return;

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
        renderFencePrepared = true;
    };

    // Once generated-image acquisition has timed out, probe availability
    // before scheduling more output work. If Gamescope still has no image, the
    // real frame is copied and presented below while a shared history pre-pass
    // keeps temporal features and timeline indices current.
    if (configuredAcquireTimeout && this->generatedImageAcquireBackoff &&
            generatedFrameCount > 0) {
        prepareRenderFence();

        auto& recoveryPass = this->passes.front();
        uint32_t recoveryImageIndex{};
        const auto now = DiagnosticsClock::now();
        boundedRecoveryProbe = !this->generatedImageAcquireLastBoundedProbe ||
            now - *this->generatedImageAcquireLastBoundedProbe >=
                generatedImageAcquireBoundedProbeInterval;
        const uint64_t recoveryTimeout = boundedRecoveryProbe ? *configuredAcquireTimeout : 0;
        if (boundedRecoveryProbe)
            this->generatedImageAcquireLastBoundedProbe = now;
        const auto acquireStarted = startPresentDiagnostic();
        const auto acquireResult = vk.df().AcquireNextImageKHR(vk.dev(), swapchain,
            recoveryTimeout, recoveryPass.acquireSemaphore.handle(), VK_NULL_HANDLE,
            &recoveryImageIndex
        );

        if (acquireResult == VK_TIMEOUT || acquireResult == VK_NOT_READY) {
            if (!this->generatedImageAcquireBypassCount || boundedRecoveryProbe) {
                logSlowPresentOperation(
                    "acquire-generated-image", this->fidx, this->idx, acquireStarted, acquireResult,
                    0, recoveryImageIndex
                );
            }
            bypassGeneratedFrames = true;
            generatedImageUnavailable = true;
        } else if (acquireResult == VK_SUCCESS || acquireResult == VK_SUBOPTIMAL_KHR) {
            logSlowPresentOperation(
                "acquire-generated-image", this->fidx, this->idx, acquireStarted, acquireResult,
                0, recoveryImageIndex
            );
            this->generatedImageAcquireBackoff = false;
            this->generatedImageAcquireLastBoundedProbe.reset();
            const size_t recoveryWarmupFrames = this->profile.adaptive
                ? adaptiveHistoryWarmupFrames
                : 0;
            logPresentRecovery(
                this->fidx, this->idx, 0, recoveryImageIndex,
                this->generatedImageAcquireBypassCount,
                boundedRecoveryProbe ? "bounded-retry" : "nonblocking-retry",
                recoveryWarmupFrames
            );
            this->generatedImageAcquireBypassCount = 0;
            this->resetAdaptiveScheduler(DiagnosticsClock::now());
            if (recoveryWarmupFrames) {
                // The successful probe owns a swapchain image, so the first
                // warm-up frame must present it even though generated output
                // remains disabled. Copy the real image into it below, then
                // refresh all temporal slots before trying generation again.
                recoveryWarmupImage = recoveryImageIndex;
                this->adaptiveHistoryWarmupRemaining = recoveryWarmupFrames;
                this->adaptiveHistoryWarmupIsRecovery = true;
                bypassGeneratedFrames = true;
            } else {
                preacquiredGeneratedImage = recoveryImageIndex;
            }
        } else {
            logSlowPresentOperation(
                "acquire-generated-image", this->fidx, this->idx, acquireStarted, acquireResult,
                0, recoveryImageIndex
            );
            throw ls::vulkan_error(acquireResult, "vkAcquireNextImageKHR() failed");
        }
    }

    // schedule frame generation
    if (!bypassGeneratedFrames) {
        const auto scheduleStarted = startPresentDiagnostic();
        try {
            if (this->profile.adaptive)
                this->instance.get().scheduleFrames(this->ctx.get(), generatedTimestamps);
            else
                this->instance.get().scheduleFrames(this->ctx.get());
        } catch (const std::exception& e) {
            throw ls::error("failed to schedule frames", e);
        }
        logSlowPresentOperation("schedule-frames", this->fidx, this->idx, scheduleStarted);
    }

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
    prepareRenderFence();

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

    if (bypassGeneratedFrames) {
        const uint64_t sourceTimelineValue = this->idx - 1;
        auto& fallbackPass = this->passes.front();
        auto& fallbackSemaphores = this->postCopySemaphores.at(
            this->idx % this->postCopySemaphores.size()
        );
        auto& fallbackSemaphore = fallbackSemaphores.second;

        auto& fallbackCommandBuffer = fallbackPass.commandBuffer;
        fallbackCommandBuffer.begin(vk);
        if (recoveryWarmupImage) {
            const auto& recoveryImage = this->info.images.at(*recoveryWarmupImage);
            fallbackCommandBuffer.blitImage(vk,
                {
                    barrierHelper(swapchainImage,
                        VK_ACCESS_MEMORY_READ_BIT,
                        VK_ACCESS_TRANSFER_READ_BIT,
                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                    ),
                    barrierHelper(recoveryImage,
                        VK_ACCESS_NONE,
                        VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                    ),
                },
                { swapchainImage, recoveryImage },
                this->info.extent,
                {
                    barrierHelper(swapchainImage,
                        VK_ACCESS_TRANSFER_READ_BIT,
                        VK_ACCESS_MEMORY_READ_BIT,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                    ),
                    barrierHelper(recoveryImage,
                        VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_ACCESS_MEMORY_READ_BIT,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                    ),
                }
            );
        }
        fallbackCommandBuffer.end(vk);
        const std::vector<VkSemaphore> fallbackWaitSemaphores = recoveryWarmupImage
            ? std::vector<VkSemaphore>{fallbackPass.acquireSemaphore.handle()}
            : std::vector<VkSemaphore>{};
        const std::vector<VkSemaphore> fallbackSignalSemaphores = recoveryWarmupImage
            ? std::vector<VkSemaphore>{
                fallbackSemaphores.first.handle(), fallbackSemaphore.handle()
            }
            : std::vector<VkSemaphore>{fallbackSemaphore.handle()};
        fallbackCommandBuffer.submit(vk,
            fallbackWaitSemaphores, this->syncSemaphore->handle(), sourceTimelineValue,
            fallbackSignalSemaphores, VK_NULL_HANDLE, 0,
            this->renderFence->handle()
        );

        try {
            this->instance.get().scheduleFrameHistory(this->ctx.get());
        } catch (const std::exception& e) {
            throw ls::error("failed to maintain frame history", e);
        }
        if (generatedImageUnavailable &&
                (!this->generatedImageAcquireBypassCount || boundedRecoveryProbe)) {
            logPresentFallback(
                this->fidx, this->idx, 0, generatedFrameCount, sourceTimelineValue,
                boundedRecoveryProbe ? "bounded-retry" : "nonblocking-retry", "history-only"
            );
        }
        if (generatedImageUnavailable)
            this->generatedImageAcquireBypassCount++;

        if (this->adaptiveHistoryWarmupRemaining) {
            logHistoryWarmup(
                this->fidx, this->idx,
                this->adaptiveHistoryWarmupRemaining,
                this->adaptiveHistoryWarmupIsRecovery,
                recoveryWarmupImage
            );
            this->adaptiveHistoryWarmupRemaining--;
            if (!this->adaptiveHistoryWarmupRemaining)
                this->adaptiveHistoryWarmupIsRecovery = false;
            this->resetAdaptiveScheduler(DiagnosticsClock::now());
        }

        void* originalNextChain = next_chain;
        if (recoveryWarmupImage) {
            const VkSemaphore recoveryWaitSemaphore = fallbackSemaphores.first.handle();
            const VkPresentInfoKHR recoveryPresentInfo{
                .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                .pNext = next_chain,
                .waitSemaphoreCount = 1,
                .pWaitSemaphores = &recoveryWaitSemaphore,
                .swapchainCount = 1,
                .pSwapchains = &swapchain,
                .pImageIndices = &*recoveryWarmupImage,
            };
            const auto recoveryPresentStarted = startPresentDiagnostic();
            const auto recoveryResult = vk.df().QueuePresentKHR(queue, &recoveryPresentInfo);
            logSlowPresentOperation(
                "present-recovery-warmup-image", this->fidx, this->idx,
                recoveryPresentStarted, recoveryResult, 0, *recoveryWarmupImage
            );
            if (recoveryResult != VK_SUCCESS && recoveryResult != VK_SUBOPTIMAL_KHR)
                throw ls::vulkan_error(recoveryResult, "vkQueuePresentKHR() failed");
            originalNextChain = nullptr;
        }

        const auto res = presentOriginalImage(fallbackSemaphore.handle(), originalNextChain);
        if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
            throw ls::vulkan_error(res, "vkQueuePresentKHR() failed");

        logSlowPresentOperation("present-total", this->fidx, this->idx, presentStarted, res);
        this->fidx++;
        return res;
    }

    for (size_t i = 0; i < generatedFrameCount; i++) {
        auto& pcs = this->postCopySemaphores.at(this->idx % this->postCopySemaphores.size());
        auto& destinationImage = this->destinationImages.at(i);
        auto& pass = this->passes.at(i);

        // acquire swapchain image
        uint32_t aqImageIdx{};
        VkResult res{};
        const bool usePreacquiredImage = i == 0 && preacquiredGeneratedImage.has_value();
        if (usePreacquiredImage) {
            aqImageIdx = *preacquiredGeneratedImage;
            res = VK_SUCCESS;
        } else {
            const auto acquireStarted = startPresentDiagnostic();
            const uint64_t acquireTimeout = configuredAcquireTimeout
                ? *configuredAcquireTimeout
                : UINT64_MAX;
            res = vk.df().AcquireNextImageKHR(vk.dev(), swapchain,
                acquireTimeout, pass.acquireSemaphore.handle(),
                VK_NULL_HANDLE,
                &aqImageIdx
            );
            logSlowPresentOperation(
                "acquire-generated-image", this->fidx, this->idx, acquireStarted, res,
                i, aqImageIdx
            );
        }
        if (configuredAcquireTimeout && (res == VK_TIMEOUT || res == VK_NOT_READY)) {
            // Gamescope can temporarily stop releasing the extra swapchain images used for generated frames while
            // an overlay is visible. Do not block the game indefinitely. The backend has already scheduled every
            // generated frame for this sequence, so wait for its final timeline value before presenting the original
            // image and advancing both sides to the next sequence.
            const size_t skippedFrames = generatedFrameCount - i;
            const uint64_t finalGeneratedTimelineValue = this->idx + skippedFrames - 1;
            auto& fallbackSemaphore = pcs.second;
            this->generatedImageAcquireBackoff = true;
            this->generatedImageAcquireBypassCount = 0;
            this->generatedImageAcquireLastBoundedProbe = DiagnosticsClock::now();
            this->adaptiveHistoryWarmupRemaining = 0;
            this->adaptiveHistoryWarmupIsRecovery = false;
            this->resetAdaptiveScheduler(this->generatedImageAcquireLastBoundedProbe.value());

            auto& fallbackCommandBuffer = pass.commandBuffer;
            fallbackCommandBuffer.begin(vk);
            fallbackCommandBuffer.end(vk);
            fallbackCommandBuffer.submit(vk,
                {}, this->syncSemaphore->handle(), finalGeneratedTimelineValue,
                { fallbackSemaphore.handle() }, VK_NULL_HANDLE, 0,
                this->renderFence->handle()
            );

            logPresentFallback(
                this->fidx, this->idx, i, skippedFrames, finalGeneratedTimelineValue,
                "initial-timeout", "scheduled"
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
        this->generatedImageAcquireBackoff = false;
        this->generatedImageAcquireLastBoundedProbe.reset();

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
            i == generatedFrameCount - 1 ? this->renderFence->handle() : VK_NULL_HANDLE
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
