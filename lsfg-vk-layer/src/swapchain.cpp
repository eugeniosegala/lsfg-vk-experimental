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

    constexpr size_t adaptiveCapacityMultiplier = 4;
    constexpr size_t adaptiveHistoryWarmupFrames = 3;
    constexpr double adaptiveMinimumBaseFps = 10.0;
    constexpr double adaptiveIntervalSmoothing = 0.25;
    constexpr double adaptiveCadenceDropRatio = 2.0;
    constexpr size_t adaptiveCadenceDropFrameCount = 3;
    constexpr double adaptiveRampThroughputTolerance = 0.95;
    constexpr double adaptiveRampBaseCollapseRatio = 0.70;
    constexpr double adaptiveRampMarginalGain = 1.15;
    constexpr double adaptiveBridgeMinimumOutputRetention = 0.85;
    constexpr double adaptiveBridgeMinimumBaseRetention = 0.40;
    constexpr double adaptiveBridgeTargetDeficitRatio = 0.90;
    // A constant cadence is worthwhile only when it does not ask the engine to
    // produce substantially more frames than the requested output target. This
    // covers common display divisors such as 60 -> 90 (120 internal FPS), with
    // enough tolerance for smoothed frame-time noise, while excluding expensive
    // cases such as 100 -> 120 (200 internal FPS). Once validated, a slightly
    // wider bound prevents ordinary scene variation from immediately undoing it.
    constexpr double adaptiveStableCadenceMaximumProbeOvershootRatio = 1.40;
    constexpr double adaptiveStableCadenceMaximumRetainedOvershootRatio = 1.50;
    constexpr double adaptiveStableCadenceMinimumTargetRatio = 0.98;
    constexpr double adaptiveStableCadenceMinimumBaseRetention = 0.74;
    // Stable cadence is a consistency preference, not a reason to replace a
    // substantially lighter fractional schedule. Require strict Adaptive to
    // already need at least 95% of the corresponding constant output count.
    constexpr double adaptiveStableCadenceMinimumDemandRatio = 0.95;
    constexpr auto adaptiveStabilizationDuration = std::chrono::seconds(1);
    // A recreated generated-frame swapchain can take longer than an ordinary
    // cadence discontinuity to settle in Gamescope.
    constexpr auto adaptiveRecoveryStabilizationDuration = std::chrono::seconds(3);
    constexpr auto adaptiveRampEvaluationDuration = std::chrono::seconds(1);
    constexpr auto adaptiveRampStepDelay = std::chrono::milliseconds(250);
    constexpr auto adaptiveRampFirstRetryDelay = std::chrono::seconds(5);
    constexpr auto adaptiveRampSecondRetryDelay = std::chrono::seconds(15);
    constexpr auto adaptiveRampThirdRetryDelay = std::chrono::seconds(30);
    constexpr auto adaptiveRampMaximumRetryDelay = std::chrono::seconds(60);
    constexpr double adaptiveRampEarlyRetryBaseImprovement = 1.15;
    constexpr auto adaptiveRecoveryHigherProbeDelay = std::chrono::seconds(5);
    constexpr auto adaptiveStableCadenceEvaluationDuration = std::chrono::seconds(1);
    constexpr auto adaptiveStableCadenceExitGraceDuration = std::chrono::milliseconds(500);
    constexpr auto adaptiveStableCadenceRetryDelay = std::chrono::seconds(5);
    constexpr auto adaptiveStableCadenceStrictSettlingDuration = std::chrono::seconds(2);
    // A severe collapse after a validated cadence can otherwise become a
    // feedback loop: the maximum allowed generated load keeps the real rate
    // low, so the target remains unreachable. Briefly measure the real-only
    // path, then resume strict scheduling or probe one higher allowed level.
    constexpr double adaptiveRescueBaseCollapseRatio = 0.78;
    constexpr double adaptiveRescueOutputCollapseRatio = 0.80;
    constexpr double adaptiveRescueRecoveredBaseRatio = 0.90;
    constexpr auto adaptiveRescueMeasurementDuration = std::chrono::seconds(1);
    constexpr auto adaptiveRescueCooldown = std::chrono::seconds(15);
    // Overlay/focus discontinuities bypass the ordinary collapse detector
    // because the raw cadence stalls before a smoothed sample can be scored.
    // Keep the last proven level, wait for real-only cadence to recover, and
    // restore it only after a bounded stable measurement.
    constexpr double adaptiveDiscontinuityRecoveredBaseRatio = 0.90;
    constexpr auto adaptiveDiscontinuityStableDuration = std::chrono::seconds(1);
    constexpr auto adaptiveDiscontinuityMaximumDuration = std::chrono::seconds(5);
    constexpr auto adaptiveFailedProbeCooldown = std::chrono::seconds(15);
    constexpr auto adaptiveStableRearmDuration = std::chrono::seconds(2);
    constexpr auto adaptiveRecreationCooldown = std::chrono::seconds(5);

    size_t generatedFrameCapacity(const ls::GameConf& profile) {
        const size_t multiplier = profile.adaptive
            ? adaptiveCapacityMultiplier
            : profile.multiplier;
        return multiplier - 1;
    }

    std::chrono::steady_clock::duration adaptiveRampRetryDelayForFailures(
            const size_t failures) {
        if (failures <= 1)
            return adaptiveRampFirstRetryDelay;
        if (failures == 2)
            return adaptiveRampSecondRetryDelay;
        if (failures == 3)
            return adaptiveRampThirdRetryDelay;
        return adaptiveRampMaximumRetryDelay;
    }

    bool presentDiagnosticsEnabled() {
        static const bool enabled = [] {
            const char* value = std::getenv("LSFGVK_PRESENT_DIAGNOSTICS");
            return value && std::string_view(value) != "0";
        }();
        return enabled;
    }

    bool presentRecoveryRecreateEnabled() {
        static const bool enabled = [] {
            const char* value = std::getenv("LSFGVK_PRESENT_RECOVERY_RECREATE");
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
            std::string_view acquireMode, size_t warmupFrames,
            bool requestSwapchainRecreation) {
        if (!presentDiagnosticsEnabled())
            return;

        std::cerr << "lsfg-vk: present diagnostics: operation="
                  << ((warmupFrames || requestSwapchainRecreation)
                      ? "generated-image-recovered"
                      : "resume-generated-frames")
                  << " frame=" << frameIndex
                  << " sequence=" << sequenceIndex
                  << " pass=" << passIndex
                  << " image=" << imageIndex
                  << " acquire_mode=" << acquireMode
                  << " bypassed_frames=" << bypassedFrames;
        if (warmupFrames)
            std::cerr << " recovery_warmup_frames=" << warmupFrames;
        if (requestSwapchainRecreation)
            std::cerr << " recovery_action=swapchain-recreate";
        std::cerr << '\n';
    }

    void logSwapchainRecreation(size_t frameIndex, size_t sequenceIndex,
            std::string_view reason) {
        if (!presentDiagnosticsEnabled())
            return;

        std::cerr << "lsfg-vk: present diagnostics: operation=request-swapchain-recreation"
                  << " frame=" << frameIndex
                  << " sequence=" << sequenceIndex
                  << " reason=" << reason << '\n';
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

    void logAdaptiveStabilization(std::string_view reason,
            const std::chrono::steady_clock::duration duration) {
        if (!presentDiagnosticsEnabled())
            return;

        std::cerr << "lsfg-vk: present diagnostics: operation=adaptive-stabilization"
                  << " reason=" << reason
                  << " duration_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         duration
                     ).count()
                  << '\n';
    }

    void logAdaptiveRamp(size_t previousLimit, size_t newLimit, double baseFps) {
        if (!presentDiagnosticsEnabled())
            return;

        std::cerr << "lsfg-vk: present diagnostics: operation=adaptive-ramp"
                  << " previous_generated_limit=" << previousLimit
                  << " generated_limit=" << newLimit
                  << " base_fps=" << baseFps << '\n';
    }

    void logAdaptiveRampResult(bool accepted, size_t previousLimit,
            size_t testedLimit, double previousBaseFps, double currentBaseFps,
            double previousOutputFps, double currentOutputFps) {
        if (!presentDiagnosticsEnabled())
            return;

        std::cerr << "lsfg-vk: present diagnostics: operation="
                  << (accepted ? "adaptive-ramp-accepted" : "adaptive-load-shed")
                  << " previous_generated_limit=" << previousLimit
                  << " tested_generated_limit=" << testedLimit
                  << " previous_base_fps=" << previousBaseFps
                  << " current_base_fps=" << currentBaseFps
                  << " previous_output_fps=" << previousOutputFps
                  << " current_output_fps=" << currentOutputFps << '\n';
    }

    void logAdaptiveBridge(size_t previousLimit, size_t testedLimit,
            size_t bridgeLimit, double previousBaseFps, double currentBaseFps,
            double previousOutputFps, double currentOutputFps) {
        if (!presentDiagnosticsEnabled())
            return;

        std::cerr << "lsfg-vk: present diagnostics: operation=adaptive-bridge"
                  << " previous_generated_limit=" << previousLimit
                  << " tested_generated_limit=" << testedLimit
                  << " bridge_generated_limit=" << bridgeLimit
                  << " previous_base_fps=" << previousBaseFps
                  << " current_base_fps=" << currentBaseFps
                  << " previous_output_fps=" << previousOutputFps
                  << " current_output_fps=" << currentOutputFps << '\n';
    }

    void logAdaptiveBridgeResult(bool accepted, size_t baselineLimit,
            size_t testedLimit, double baselineBaseFps, double currentBaseFps,
            double baselineOutputFps, double currentOutputFps) {
        if (!presentDiagnosticsEnabled())
            return;

        std::cerr << "lsfg-vk: present diagnostics: operation="
                  << (accepted ? "adaptive-bridge-accepted" : "adaptive-bridge-rejected")
                  << " baseline_generated_limit=" << baselineLimit
                  << " tested_generated_limit=" << testedLimit
                  << " baseline_base_fps=" << baselineBaseFps
                  << " current_base_fps=" << currentBaseFps
                  << " baseline_output_fps=" << baselineOutputFps
                  << " current_output_fps=" << currentOutputFps << '\n';
    }

    void logAdaptiveProbeAborted(std::string_view reason, size_t testedLimit) {
        if (!presentDiagnosticsEnabled())
            return;

        std::cerr << "lsfg-vk: present diagnostics: operation=adaptive-probe-aborted"
                  << " reason=" << reason
                  << " tested_generated_limit=" << testedLimit << '\n';
    }

    void logAdaptiveRearm(std::string_view operation, std::string_view reason,
            size_t failures, size_t fallbackLimit) {
        if (!presentDiagnosticsEnabled())
            return;

        std::cerr << "lsfg-vk: present diagnostics: operation=" << operation
                  << " reason=" << reason
                  << " consecutive_failures=" << failures;
        if (operation == "adaptive-rearm-scheduled") {
            std::cerr << " cooldown_ms="
                      << std::chrono::duration_cast<std::chrono::milliseconds>(
                             adaptiveFailedProbeCooldown
                         ).count()
                      << " stable_required_ms="
                      << std::chrono::duration_cast<std::chrono::milliseconds>(
                             adaptiveStableRearmDuration
                         ).count()
                      << " fallback_generated_limit=" << fallbackLimit;
        }
        std::cerr << '\n';
    }

    void logAdaptiveRampBackoff(size_t testedLimit, size_t failures,
            double baselineBaseFps,
            const std::chrono::steady_clock::duration delay) {
        if (!presentDiagnosticsEnabled())
            return;

        std::cerr << "lsfg-vk: present diagnostics: operation=adaptive-ramp-backoff"
                  << " tested_generated_limit=" << testedLimit
                  << " consecutive_failures=" << failures
                  << " baseline_base_fps=" << baselineBaseFps
                  << " retry_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         delay
                     ).count()
                  << '\n';
    }

    void logAdaptiveRampEarlyRetry(size_t testedLimit, double failedBaseFps,
            double currentBaseFps) {
        if (!presentDiagnosticsEnabled())
            return;

        std::cerr << "lsfg-vk: present diagnostics: operation=adaptive-ramp-early-retry"
                  << " tested_generated_limit=" << testedLimit
                  << " failed_base_fps=" << failedBaseFps
                  << " current_base_fps=" << currentBaseFps << '\n';
    }

    void logAdaptiveRecoveryResume(size_t generationLimit,
            const std::chrono::steady_clock::duration higherProbeDelay,
            std::string_view reason) {
        if (!presentDiagnosticsEnabled())
            return;

        std::cerr << "lsfg-vk: present diagnostics: operation="
                  << "adaptive-recovery-resume-scheduled"
                  << " generated_limit=" << generationLimit
                  << " higher_probe_delay_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         higherProbeDelay
                     ).count()
                  << " reason=" << reason << '\n';
    }

    void logAdaptiveStableCadence(std::string_view operation, size_t generatedLimit,
            double baselineBaseFps, double currentBaseFps,
            std::string_view reason = {}) {
        if (!presentDiagnosticsEnabled())
            return;

        std::cerr << "lsfg-vk: present diagnostics: operation=" << operation
                  << " generated_limit=" << generatedLimit
                  << " baseline_base_fps=" << baselineBaseFps
                  << " current_base_fps=" << currentBaseFps
                  << " projected_output_fps="
                  << currentBaseFps * static_cast<double>(generatedLimit + 1);
        if (!reason.empty())
            std::cerr << " reason=" << reason;
        std::cerr << '\n';
    }

    void logAdaptiveRescueStart(size_t generatedLimit,
            double baselineBaseFps, double currentBaseFps,
            double projectedOutputFps) {
        if (!presentDiagnosticsEnabled())
            return;

        std::cerr << "lsfg-vk: present diagnostics: operation=adaptive-rescue-start"
                  << " generated_limit=" << generatedLimit
                  << " baseline_base_fps=" << baselineBaseFps
                  << " current_base_fps=" << currentBaseFps
                  << " projected_output_fps=" << projectedOutputFps
                  << " measurement_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         adaptiveRescueMeasurementDuration
                     ).count()
                  << '\n';
    }

    void logAdaptiveRescueComplete(size_t previousLimit,
            size_t requestedLimit, size_t configuredLimit,
            double baselineBaseFps, double measuredBaseFps,
            std::string_view decision) {
        if (!presentDiagnosticsEnabled())
            return;

        std::cerr << "lsfg-vk: present diagnostics: operation=adaptive-rescue-complete"
                  << " previous_generated_limit=" << previousLimit
                  << " requested_generated_limit=" << requestedLimit
                  << " configured_generated_limit=" << configuredLimit
                  << " baseline_base_fps=" << baselineBaseFps
                  << " measured_base_fps=" << measuredBaseFps
                  << " decision=" << decision
                  << " cooldown_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         adaptiveRescueCooldown
                     ).count()
                  << '\n';
    }

    void logAdaptiveDiscontinuityRecoveryStart(size_t generationLimit,
            double baselineBaseFps, std::string_view reason,
            std::chrono::steady_clock::duration maximumDuration) {
        if (!presentDiagnosticsEnabled())
            return;

        std::cerr << "lsfg-vk: present diagnostics: operation="
                  << "adaptive-discontinuity-recovery-start"
                  << " generated_limit=" << generationLimit
                  << " baseline_base_fps=" << baselineBaseFps
                  << " reason=" << reason
                  << " maximum_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         maximumDuration
                     ).count()
                  << '\n';
    }

    void logAdaptiveDiscontinuityRecoveryComplete(size_t generationLimit,
            double baselineBaseFps, double measuredBaseFps,
            std::string_view decision) {
        if (!presentDiagnosticsEnabled())
            return;

        std::cerr << "lsfg-vk: present diagnostics: operation="
                  << "adaptive-discontinuity-recovery-complete"
                  << " generated_limit=" << generationLimit
                  << " baseline_base_fps=" << baselineBaseFps
                  << " measured_base_fps=" << measuredBaseFps
                  << " decision=" << decision << '\n';
    }

    void logAdaptiveDiscontinuitySoftRecovery(size_t generationLimit) {
        if (!presentDiagnosticsEnabled())
            return;

        std::cerr << "lsfg-vk: present diagnostics: operation="
                  << "adaptive-discontinuity-soft-recovery"
                  << " generated_limit=" << generationLimit
                  << " action=history-warmup"
                  << '\n';
    }

    void logSwapchainRecreationSuppressed(double remainingMs) {
        if (!presentDiagnosticsEnabled())
            return;

        std::cerr << "lsfg-vk: present diagnostics: operation=swapchain-recreation-suppressed"
                  << " reason=cooldown"
                  << " remaining_ms=" << remainingMs << '\n';
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
            ls::GameConf profile, SwapchainInfo info,
            AdaptiveRecoveryState* recoveryState, bool recoveryContext,
            const size_t recoveryGenerationLimit,
            const bool discontinuityRecoveryContext,
            const double discontinuityBaselineBaseFps,
            const std::optional<std::chrono::steady_clock::time_point>
                discontinuityDeadline,
            const bool discontinuitySoftRecoveryAttempted) :
        instance(backend),
        adaptiveRecoveryState(recoveryState),
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
                  << " fps, maximum multiplier="
                  << this->profile.adaptive_max_multiplier
                  << "x, stable cadence="
                  << (this->profile.adaptive_stable_cadence ? "enabled" : "disabled")
                  << '\n';
        this->beginAdaptiveStabilization(
            DiagnosticsClock::now(),
            recoveryContext ? "swapchain-recreation" : "startup"
        );
        if (recoveryContext) {
            if (discontinuityRecoveryContext) {
                this->beginAdaptiveDiscontinuityRecovery(
                    DiagnosticsClock::now(),
                    recoveryGenerationLimit,
                    discontinuityBaselineBaseFps,
                    discontinuityDeadline,
                    discontinuitySoftRecoveryAttempted,
                    "swapchain-recreation"
                );
            } else {
                this->restoreAdaptiveGenerationLimit(
                    DiagnosticsClock::now(),
                    recoveryGenerationLimit,
                    "swapchain-recreation"
                );
            }
        }
    }
}

std::vector<float> Swapchain::generatedFrameTimestamps(
        const std::chrono::steady_clock::time_point now) {
    if (this->generatedImageAcquireBackoff) {
        // Keep probing for one generated-image slot without advancing ramp,
        // stable-cadence, or load-shed policy while the generated workload is
        // deliberately bypassed. Evaluating a multiplier during this phase
        // would measure only the real-frame path and could falsely accept it.
        return {0.5F};
    }

    if (!this->adaptiveLastRealFrame) {
        this->adaptiveLastRealFrame = now;
        return {};
    }

    const double rawIntervalSeconds = std::chrono::duration<double>(
        now - *this->adaptiveLastRealFrame
    ).count();
    this->adaptiveLastRealFrame = now;

    // Loading screens, suspension and base rates below 10 FPS do not provide
    // useful motion history. Present real frames until cadence has been stable
    // for a bounded interval instead of immediately reapplying model load.
    if (rawIntervalSeconds <= 0.0 ||
            rawIntervalSeconds > 1.0 / adaptiveMinimumBaseFps) {
        this->beginAdaptiveStabilization(now, "cadence-stall");
        return {};
    }

    // A sustained interval jump is normally a menu, focus or display-mode
    // transition. Three samples avoid treating an isolated gameplay hitch as
    // a compositor discontinuity.
    const bool cadenceDropCandidate =
        this->adaptiveSmoothedIntervalSeconds > 0.0 &&
            rawIntervalSeconds >=
                this->adaptiveSmoothedIntervalSeconds * adaptiveCadenceDropRatio;
    if (cadenceDropCandidate) {
        this->adaptiveStableRearmSince.reset();
        this->adaptiveCadenceDropFrames++;
    } else {
        this->adaptiveCadenceDropFrames = 0;
    }
    if (this->adaptiveCadenceDropFrames >= adaptiveCadenceDropFrameCount) {
        this->beginAdaptiveStabilization(now, "cadence-drop");
        return {};
    }

    if (this->adaptiveSmoothedIntervalSeconds == 0.0) {
        this->adaptiveSmoothedIntervalSeconds = rawIntervalSeconds;
    } else if (!cadenceDropCandidate) {
        // Keep the pre-disruption baseline while confirming a sustained drop.
        // Otherwise smoothing the first slow samples raises the comparison
        // threshold and can hide the third confirming frame.
        this->adaptiveSmoothedIntervalSeconds =
            (1.0 - adaptiveIntervalSmoothing) * this->adaptiveSmoothedIntervalSeconds +
            adaptiveIntervalSmoothing * rawIntervalSeconds;
    }

    const double baseFps = 1.0 / this->adaptiveSmoothedIntervalSeconds;
    if (this->adaptiveDiscontinuityRecoveryDeadline) {
        const size_t recoveryLimit = this->adaptiveDiscontinuityGenerationLimit;
        const double baselineBaseFps =
            this->adaptiveDiscontinuityBaselineBaseFps;
        const bool initialStabilizationComplete =
            !this->adaptiveStabilizationUntil ||
            now >= *this->adaptiveStabilizationUntil;
        const bool baseRecovered = baselineBaseFps > 0.0 &&
            baseFps >= baselineBaseFps *
                adaptiveDiscontinuityRecoveredBaseRatio;

        if (baseRecovered) {
            if (!this->adaptiveDiscontinuityStableSince)
                this->adaptiveDiscontinuityStableSince = now;
        } else {
            this->adaptiveDiscontinuityStableSince.reset();
        }

        const bool recoveredCadenceStable = initialStabilizationComplete &&
            this->adaptiveDiscontinuityStableSince &&
            now - *this->adaptiveDiscontinuityStableSince >=
                adaptiveDiscontinuityStableDuration;
        const bool recoveryExpired =
            now >= *this->adaptiveDiscontinuityRecoveryDeadline;

        if (recoveredCadenceStable || recoveryExpired) {
            const std::string_view decision = recoveredCadenceStable
                ? "restore-validated-level"
                : "timeout-ramp-from-zero";
            this->adaptiveDiscontinuityRecoveryDeadline.reset();
            this->adaptiveDiscontinuityStableSince.reset();
            this->adaptiveDiscontinuityGenerationLimit = 0;
            this->adaptiveDiscontinuityBaselineBaseFps = 0.0;
            this->adaptiveDiscontinuitySoftRecoveryAttempted = false;
            this->adaptiveStabilizationUntil.reset();
            this->adaptiveOutputCredit = 0.0;
            if (recoveredCadenceStable) {
                this->restoreAdaptiveGenerationLimit(
                    now, recoveryLimit, "cadence-discontinuity"
                );
                if (this->profile.adaptive_stable_cadence) {
                    this->adaptiveStableCadenceRetryAt =
                        now + adaptiveStableCadenceStrictSettlingDuration;
                }
            } else {
                this->adaptiveGenerationLimit = 0;
                this->adaptiveRampEvaluationAt.reset();
                this->adaptiveBridgeActive = false;
                this->adaptiveBridgeBaselineLimit = 0;
                this->adaptiveBridgeBaselineBaseFps = 0.0;
                this->adaptiveRearmRequired = false;
                this->adaptiveRearmNotBefore.reset();
                this->adaptiveStableRearmSince.reset();
                this->adaptiveRearmFallbackLimit = 0;
                this->adaptiveConsecutiveProbeFailures = 0;
                this->adaptiveLastFailedRampLimit = 0;
                this->adaptiveConsecutiveRampFailures = 0;
                this->adaptiveFailedRampBaselineBaseFps = 0.0;
                this->adaptiveNextRampAt = now + adaptiveRampStepDelay;
            }
            logAdaptiveDiscontinuityRecoveryComplete(
                recoveryLimit,
                baselineBaseFps,
                baseFps,
                decision
            );
            // Keep this transition frame real-only. The restored or freshly
            // ramped policy starts on the following real frame.
            return {};
        }

        this->adaptiveOutputCredit = 0.0;
        if (presentDiagnosticsEnabled() &&
                (!this->adaptiveLastDiagnostic ||
                 now - *this->adaptiveLastDiagnostic >= std::chrono::seconds(1))) {
            this->adaptiveLastDiagnostic = now;
            std::cerr << "lsfg-vk: present diagnostics: operation=adaptive-plan"
                      << " base_fps=" << baseFps
                      << " target_fps=" << this->profile.target_fps
                      << " generated=0 max_generated=0"
                      << " phase=discontinuity-recovery"
                      << " recovery_generated_limit=" << recoveryLimit
                      << '\n';
        }
        return {};
    }

    if (this->adaptiveRescueUntil) {
        if (now < *this->adaptiveRescueUntil) {
            this->adaptiveOutputCredit = 0.0;
            if (presentDiagnosticsEnabled() &&
                    (!this->adaptiveLastDiagnostic ||
                     now - *this->adaptiveLastDiagnostic >= std::chrono::seconds(1))) {
                this->adaptiveLastDiagnostic = now;
                std::cerr << "lsfg-vk: present diagnostics: operation=adaptive-plan"
                          << " base_fps=" << baseFps
                          << " target_fps=" << this->profile.target_fps
                          << " generated=0 max_generated=0"
                          << " phase=rescue\n";
            }
            return {};
        }

        const size_t configuredLimit = std::min(
            this->destinationImages.size(),
            this->profile.adaptive_max_multiplier - 1
        );
        const size_t previousLimit = std::min(
            this->adaptiveRescuePreviousLimit, configuredLimit
        );
        const double baselineBaseFps = this->adaptiveRescueBaselineBaseFps;
        const size_t requiredOutputs = std::max<size_t>(
            1,
            static_cast<size_t>(std::ceil(
                static_cast<double>(this->profile.target_fps) / baseFps - 1e-9
            ))
        );
        const size_t requiredLimit = requiredOutputs - 1;
        const size_t requestedLimit = std::min(requiredLimit, configuredLimit);
        const bool baseRecovered = baselineBaseFps > 0.0 &&
            baseFps >= baselineBaseFps * adaptiveRescueRecoveredBaseRatio;

        std::string_view decision = "resume-strict";
        this->adaptiveGenerationLimit = previousLimit;
        this->adaptiveRampEvaluationAt.reset();
        this->adaptiveBridgeActive = false;
        this->adaptiveBridgeBaselineLimit = 0;
        this->adaptiveBridgeBaselineBaseFps = 0.0;
        this->adaptiveNextRampAt.reset();
        if (baseRecovered) {
            decision = "recovered-strict";
        } else if (requestedLimit > previousLimit) {
            // updateAdaptiveGenerationLimit() below will probe only the next
            // allowed level and apply its existing throughput checks.
            this->adaptiveNextRampAt = now;
            decision = "probe-higher-limit";
        } else if (requiredLimit > configuredLimit) {
            decision = "ceiling-limited";
        }

        this->adaptiveRescueUntil.reset();
        this->adaptiveRescuePreviousLimit = 0;
        this->adaptiveRescueBaselineBaseFps = 0.0;
        this->adaptiveOutputCredit = 0.0;
        if (this->adaptiveRescueCooldownUntil)
            this->adaptiveStableCadenceRetryAt = this->adaptiveRescueCooldownUntil;
        logAdaptiveRescueComplete(
            previousLimit,
            requestedLimit,
            configuredLimit,
            baselineBaseFps,
            baseFps,
            decision
        );
    }

    if (this->adaptiveStabilizationUntil &&
            now < *this->adaptiveStabilizationUntil) {
        this->adaptiveOutputCredit = 0.0;
        if (presentDiagnosticsEnabled() &&
                (!this->adaptiveLastDiagnostic ||
                 now - *this->adaptiveLastDiagnostic >= std::chrono::seconds(1))) {
            this->adaptiveLastDiagnostic = now;
            std::cerr << "lsfg-vk: present diagnostics: operation=adaptive-plan"
                      << " base_fps=" << baseFps
                      << " target_fps=" << this->profile.target_fps
                      << " generated=0 max_generated=0"
                      << " phase=stabilizing\n";
        }
        return {};
    }
    this->adaptiveStabilizationUntil.reset();
    this->updateAdaptiveGenerationLimit(now, baseFps);

    const double desiredOutputsPerRealFrame =
        this->adaptiveSmoothedIntervalSeconds *
        static_cast<double>(this->profile.target_fps);

    const size_t maximumGeneratedFrameCount = std::min(
        {
            this->destinationImages.size(),
            this->profile.adaptive_max_multiplier - 1,
            this->adaptiveGenerationLimit,
        }
    );

    const auto stableCadenceCandidate = [&]() -> std::optional<size_t> {
        if (!this->profile.adaptive_stable_cadence)
            return std::nullopt;
        if (desiredOutputsPerRealFrame <= 1.0)
            return std::nullopt;

        const double minimumUsefulOutputFps =
            static_cast<double>(this->profile.target_fps) *
                adaptiveStableCadenceMinimumTargetRatio;
        const size_t candidateOutputs = static_cast<size_t>(std::ceil(
            minimumUsefulOutputFps / baseFps - 1e-9
        ));
        if (candidateOutputs <= 1)
            return std::nullopt;
        if (desiredOutputsPerRealFrame /
                static_cast<double>(candidateOutputs) <
                    adaptiveStableCadenceMinimumDemandRatio)
            return std::nullopt;

        const size_t candidateGenerated = candidateOutputs - 1;
        if (candidateGenerated > maximumGeneratedFrameCount)
            return std::nullopt;

        const double projectedOutputFps = baseFps *
            static_cast<double>(candidateOutputs);
        if (projectedOutputFps >
                static_cast<double>(this->profile.target_fps) *
                    adaptiveStableCadenceMaximumProbeOvershootRatio)
            return std::nullopt;

        return candidateGenerated;
    }();

    if (this->adaptiveStableCadenceLimit) {
        const size_t generatedLimit = *this->adaptiveStableCadenceLimit;
        const double targetFps = static_cast<double>(this->profile.target_fps);
        const double projectedOutputFps = baseFps *
            static_cast<double>(generatedLimit + 1);
        const bool capacityAvailable =
            generatedLimit <= maximumGeneratedFrameCount;
        const bool cadenceStillUseful =
            desiredOutputsPerRealFrame > 1.0 &&
            projectedOutputFps >=
                targetFps * adaptiveStableCadenceMinimumTargetRatio &&
            projectedOutputFps <=
                targetFps * adaptiveStableCadenceMaximumRetainedOvershootRatio;

        if (!capacityAvailable) {
            logAdaptiveStableCadence(
                "adaptive-stable-cadence-disabled",
                generatedLimit,
                this->adaptiveStableCadenceBaselineBaseFps,
                baseFps,
                "capacity-changed"
            );
            this->adaptiveStableCadenceLimit.reset();
            this->adaptiveStableCadenceEvaluationAt.reset();
            this->adaptiveStableCadenceOutsideRangeSince.reset();
            this->adaptiveStableCadenceRetryAt =
                now + adaptiveStableCadenceRetryDelay;
        } else if (!cadenceStillUseful) {
            if (!this->adaptiveStableCadenceOutsideRangeSince)
                this->adaptiveStableCadenceOutsideRangeSince = now;
            if (now - *this->adaptiveStableCadenceOutsideRangeSince >=
                    adaptiveStableCadenceExitGraceDuration) {
                const bool rescueCooldownElapsed =
                    !this->adaptiveRescueCooldownUntil ||
                    now >= *this->adaptiveRescueCooldownUntil;
                const bool severeCollapse =
                    rescueCooldownElapsed &&
                    this->adaptiveStableCadenceBaselineBaseFps > 0.0 &&
                    baseFps <= this->adaptiveStableCadenceBaselineBaseFps *
                        adaptiveRescueBaseCollapseRatio &&
                    projectedOutputFps <= targetFps *
                        adaptiveRescueOutputCollapseRatio;
                logAdaptiveStableCadence(
                    "adaptive-stable-cadence-disabled",
                    generatedLimit,
                    this->adaptiveStableCadenceBaselineBaseFps,
                    baseFps,
                    severeCollapse ? "collapse-rescue" : "outside-useful-range"
                );
                const double rescueBaselineBaseFps =
                    this->adaptiveStableCadenceBaselineBaseFps;
                this->adaptiveStableCadenceLimit.reset();
                this->adaptiveStableCadenceEvaluationAt.reset();
                this->adaptiveStableCadenceOutsideRangeSince.reset();
                this->adaptiveStableCadenceRetryAt =
                    now + adaptiveStableCadenceRetryDelay;
                if (severeCollapse) {
                    this->adaptiveRescuePreviousLimit = generatedLimit;
                    this->adaptiveRescueBaselineBaseFps = rescueBaselineBaseFps;
                    this->adaptiveRescueUntil =
                        now + adaptiveRescueMeasurementDuration;
                    this->adaptiveRescueCooldownUntil =
                        now + adaptiveRescueCooldown;
                    this->adaptiveOutputCredit = 0.0;
                    logAdaptiveRescueStart(
                        generatedLimit,
                        rescueBaselineBaseFps,
                        baseFps,
                        projectedOutputFps
                    );
                    return {};
                }
            }
        } else {
            this->adaptiveStableCadenceOutsideRangeSince.reset();
        }

        if (this->adaptiveStableCadenceLimit &&
                this->adaptiveStableCadenceEvaluationAt &&
                now >= *this->adaptiveStableCadenceEvaluationAt) {
            const size_t evaluatedGeneratedLimit =
                *this->adaptiveStableCadenceLimit;
            const double evaluatedProjectedOutputFps = baseFps *
                static_cast<double>(evaluatedGeneratedLimit + 1);
            const bool accepted =
                evaluatedProjectedOutputFps >=
                    targetFps * adaptiveStableCadenceMinimumTargetRatio &&
                evaluatedProjectedOutputFps <=
                    targetFps *
                        adaptiveStableCadenceMaximumRetainedOvershootRatio &&
                baseFps >= this->adaptiveStableCadenceBaselineBaseFps *
                    adaptiveStableCadenceMinimumBaseRetention;
            logAdaptiveStableCadence(
                accepted ? "adaptive-stable-cadence-accepted"
                         : "adaptive-stable-cadence-rejected",
                evaluatedGeneratedLimit,
                this->adaptiveStableCadenceBaselineBaseFps,
                baseFps
            );
            this->adaptiveStableCadenceEvaluationAt.reset();
            if (!accepted) {
                this->adaptiveStableCadenceLimit.reset();
                this->adaptiveStableCadenceOutsideRangeSince.reset();
                this->adaptiveStableCadenceRetryAt =
                    now + adaptiveStableCadenceRetryDelay;
            }
        }
    }

    if (!this->adaptiveStableCadenceLimit && stableCadenceCandidate &&
            !this->adaptiveRampEvaluationAt &&
            !this->adaptiveRearmRequired &&
            (!this->adaptiveRescueCooldownUntil ||
             now >= *this->adaptiveRescueCooldownUntil) &&
            (!this->adaptiveStableCadenceRetryAt ||
             now >= *this->adaptiveStableCadenceRetryAt)) {
        this->adaptiveStableCadenceLimit = *stableCadenceCandidate;
        this->adaptiveStableCadenceBaselineBaseFps = baseFps;
        this->adaptiveStableCadenceEvaluationAt =
            now + adaptiveStableCadenceEvaluationDuration;
        this->adaptiveStableCadenceOutsideRangeSince.reset();
        this->adaptiveStableCadenceRetryAt.reset();
        this->adaptiveOutputCredit = 0.0;
        logAdaptiveStableCadence(
            "adaptive-stable-cadence-probe",
            *stableCadenceCandidate,
            baseFps,
            baseFps
        );
    }

    size_t generatedFrameCount = 0;
    if (this->adaptiveStableCadenceLimit) {
        generatedFrameCount = *this->adaptiveStableCadenceLimit;
        this->adaptiveOutputCredit = 0.0;
    } else if (desiredOutputsPerRealFrame > 1.0) {
        this->adaptiveOutputCredit += desiredOutputsPerRealFrame;
        const size_t requestedOutputs = std::max<size_t>(
            1,
            static_cast<size_t>(std::floor(this->adaptiveOutputCredit + 1e-9))
        );
        generatedFrameCount = std::min(
            requestedOutputs - 1,
            maximumGeneratedFrameCount
        );
        this->adaptiveOutputCredit -= static_cast<double>(generatedFrameCount + 1);
        if (this->adaptiveOutputCredit < 0.0)
            this->adaptiveOutputCredit = 0.0;
        if (generatedFrameCount == maximumGeneratedFrameCount &&
                this->adaptiveOutputCredit >= 1.0) {
            // The requested target is currently above the configured ceiling. Keep
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
        std::cerr << "lsfg-vk: present diagnostics: operation=adaptive-plan"
                  << " base_fps=" << baseFps
                  << " target_fps=" << this->profile.target_fps
                  << " generated=" << generatedFrameCount
                  << " max_generated=" << maximumGeneratedFrameCount
                  << " configured_max_generated="
                  << this->profile.adaptive_max_multiplier - 1
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

size_t Swapchain::validatedAdaptiveGenerationLimit() const {
    if (!this->profile.adaptive)
        return 0;

    const size_t configuredLimit = std::min(
        this->destinationImages.size(),
        this->profile.adaptive_max_multiplier - 1
    );
    size_t generationLimit = this->adaptiveGenerationLimit;
    if (this->adaptiveRearmRequired) {
        generationLimit = this->adaptiveRearmFallbackLimit;
    } else if (this->adaptiveRampEvaluationAt) {
        generationLimit = this->adaptiveBridgeActive
            ? this->adaptiveBridgeBaselineLimit
            : this->adaptiveRampPreviousLimit;
    }
    return std::min(generationLimit, configuredLimit);
}

void Swapchain::restoreAdaptiveGenerationLimit(
        const std::chrono::steady_clock::time_point now,
        const size_t generationLimit,
        const std::string_view reason) {
    if (!this->profile.adaptive)
        return;

    const size_t configuredLimit = std::min(
        this->destinationImages.size(),
        this->profile.adaptive_max_multiplier - 1
    );
    const size_t restoredLimit = std::min(generationLimit, configuredLimit);

    this->adaptiveGenerationLimit = restoredLimit;
    this->adaptiveRampPreviousLimit = restoredLimit;
    this->adaptiveRampEvaluationAt.reset();
    this->adaptiveRampBaselineBaseFps = 0.0;
    this->adaptiveBridgeActive = false;
    this->adaptiveBridgeBaselineLimit = 0;
    this->adaptiveBridgeBaselineBaseFps = 0.0;
    this->adaptiveRearmRequired = false;
    this->adaptiveRearmNotBefore.reset();
    this->adaptiveStableRearmSince.reset();
    this->adaptiveRearmFallbackLimit = 0;
    this->adaptiveConsecutiveProbeFailures = 0;
    this->adaptiveLastFailedRampLimit = 0;
    this->adaptiveConsecutiveRampFailures = 0;
    this->adaptiveFailedRampBaselineBaseFps = 0.0;
    this->adaptiveOutputCredit = 0.0;

    const auto stabilizationEnd = this->adaptiveStabilizationUntil.value_or(now);
    const auto higherProbeDelay = restoredLimit > 0 && restoredLimit < configuredLimit
        ? adaptiveRecoveryHigherProbeDelay
        : DiagnosticsClock::duration::zero();
    this->adaptiveNextRampAt = stabilizationEnd + higherProbeDelay;
    logAdaptiveRecoveryResume(restoredLimit, higherProbeDelay, reason);
}

void Swapchain::beginAdaptiveDiscontinuityRecovery(
        const std::chrono::steady_clock::time_point now,
        const size_t generationLimit,
        const double baselineBaseFps,
        const std::optional<std::chrono::steady_clock::time_point> deadline,
        const bool softRecoveryAttempted,
        const std::string_view reason) {
    if (!this->profile.adaptive || generationLimit == 0 ||
            baselineBaseFps <= 0.0)
        return;

    const size_t configuredLimit = std::min(
        this->destinationImages.size(),
        this->profile.adaptive_max_multiplier - 1
    );
    this->adaptiveDiscontinuityGenerationLimit = std::min(
        generationLimit, configuredLimit
    );
    if (this->adaptiveDiscontinuityGenerationLimit == 0)
        return;

    this->adaptiveDiscontinuityBaselineBaseFps = baselineBaseFps;
    const auto minimumDeadline = now + adaptiveDiscontinuityStableDuration;
    this->adaptiveDiscontinuityRecoveryDeadline = deadline
        ? std::max(*deadline, minimumDeadline)
        : now + adaptiveDiscontinuityMaximumDuration;
    this->adaptiveDiscontinuityStableSince.reset();
    this->adaptiveDiscontinuitySoftRecoveryAttempted = softRecoveryAttempted;
    this->adaptiveOutputCredit = 0.0;

    const auto remainingDuration =
        *this->adaptiveDiscontinuityRecoveryDeadline > now
        ? *this->adaptiveDiscontinuityRecoveryDeadline - now
        : DiagnosticsClock::duration::zero();
    logAdaptiveDiscontinuityRecoveryStart(
        this->adaptiveDiscontinuityGenerationLimit,
        baselineBaseFps,
        reason,
        remainingDuration
    );
}

void Swapchain::scheduleAdaptiveRearm(
        const std::chrono::steady_clock::time_point now,
        const std::string_view reason,
        const size_t fallbackLimit) {
    this->adaptiveConsecutiveProbeFailures++;
    this->adaptiveRearmRequired = true;
    this->adaptiveRearmNotBefore = now + adaptiveFailedProbeCooldown;
    this->adaptiveStableRearmSince.reset();
    this->adaptiveRearmFallbackLimit = fallbackLimit;
    this->adaptiveNextRampAt = this->adaptiveRearmNotBefore;
    logAdaptiveRearm(
        "adaptive-rearm-scheduled",
        reason,
        this->adaptiveConsecutiveProbeFailures,
        this->adaptiveRearmFallbackLimit
    );
}

void Swapchain::beginAdaptiveStabilization(
        const std::chrono::steady_clock::time_point now,
        const std::string_view reason) {
    if (!this->profile.adaptive)
        return;

    const bool cadenceDiscontinuity =
        reason == "cadence-stall" || reason == "cadence-drop";
    if (cadenceDiscontinuity)
        this->adaptiveDiscontinuityStableSince.reset();
    if (cadenceDiscontinuity &&
            !this->adaptiveDiscontinuityRecoveryDeadline &&
            this->adaptiveSmoothedIntervalSeconds > 0.0) {
        size_t recoveryLimit = this->validatedAdaptiveGenerationLimit();
        if (this->adaptiveStableCadenceLimit) {
            recoveryLimit = std::max(
                recoveryLimit, *this->adaptiveStableCadenceLimit
            );
        }
        this->beginAdaptiveDiscontinuityRecovery(
            now,
            recoveryLimit,
            1.0 / this->adaptiveSmoothedIntervalSeconds,
            std::nullopt,
            false,
            reason
        );
    }

    const bool alreadyStabilizing = this->adaptiveStabilizationUntil &&
        now < *this->adaptiveStabilizationUntil;
    size_t rearmFallbackLimit = 0;
    if (this->adaptiveRampEvaluationAt) {
        rearmFallbackLimit = this->adaptiveBridgeActive
            ? this->adaptiveBridgeBaselineLimit
            : this->adaptiveRampPreviousLimit;
        logAdaptiveProbeAborted(reason, this->adaptiveGenerationLimit);
        this->scheduleAdaptiveRearm(
            now, "probe-interrupted", rearmFallbackLimit
        );
    } else if (this->adaptiveRearmRequired) {
        // A fresh cadence disruption restarts the stable-cadence requirement,
        // but it does not extend the already bounded cooldown indefinitely.
        this->adaptiveStableRearmSince.reset();
    }
    // Startup can include an uncapped splash screen or launcher followed by
    // normal gameplay. Do not let those first samples start a probe that the
    // gameplay transition immediately interrupts and cools down for 15 seconds.
    const auto stabilizationDuration =
        reason == "swapchain-recreation" || reason == "startup"
        ? adaptiveRecoveryStabilizationDuration
        : adaptiveStabilizationDuration;
    this->adaptiveStabilizationUntil = now + stabilizationDuration;
    this->adaptiveNextRampAt = this->adaptiveStabilizationUntil;
    if (this->adaptiveRearmNotBefore &&
            *this->adaptiveRearmNotBefore > *this->adaptiveNextRampAt) {
        this->adaptiveNextRampAt = this->adaptiveRearmNotBefore;
    }
    this->adaptiveRampEvaluationAt.reset();
    this->adaptiveGenerationLimit = 0;
    this->adaptiveRampPreviousLimit = 0;
    this->adaptiveRampBaselineBaseFps = 0.0;
    this->adaptiveBridgeActive = false;
    this->adaptiveBridgeBaselineLimit = 0;
    this->adaptiveBridgeBaselineBaseFps = 0.0;
    this->adaptiveStableCadenceLimit.reset();
    this->adaptiveStableCadenceEvaluationAt.reset();
    this->adaptiveStableCadenceOutsideRangeSince.reset();
    this->adaptiveStableCadenceRetryAt.reset();
    this->adaptiveStableCadenceBaselineBaseFps = 0.0;
    this->adaptiveRescueUntil.reset();
    this->adaptiveRescuePreviousLimit = 0;
    this->adaptiveRescueBaselineBaseFps = 0.0;
    this->adaptiveCadenceDropFrames = 0;
    this->adaptiveLastDiagnostic.reset();
    this->resetAdaptiveScheduler(now);
    if (!alreadyStabilizing)
        logAdaptiveStabilization(reason, stabilizationDuration);
}

void Swapchain::updateAdaptiveGenerationLimit(
        const std::chrono::steady_clock::time_point now,
        const double baseFps) {
    const size_t configuredLimit = std::min(
        this->destinationImages.size(),
        this->profile.adaptive_max_multiplier - 1
    );
    this->adaptiveGenerationLimit = std::min(
        this->adaptiveGenerationLimit, configuredLimit
    );

    if (this->adaptiveRearmRequired) {
        // The stabilization phase itself remains real-frame-only. Once it has
        // completed, retain the last proven level while the failed higher
        // probe cools down instead of dropping frame generation altogether.
        this->adaptiveGenerationLimit = std::min(
            this->adaptiveRearmFallbackLimit, configuredLimit
        );
        if (!this->adaptiveStableRearmSince)
            this->adaptiveStableRearmSince = now;

        const bool cooldownElapsed =
            !this->adaptiveRearmNotBefore || now >= *this->adaptiveRearmNotBefore;
        const bool cadenceStable =
            now - *this->adaptiveStableRearmSince >= adaptiveStableRearmDuration;
        if (!cooldownElapsed || !cadenceStable)
            return;

        logAdaptiveRearm(
            "adaptive-rearm-ready",
            "stable-cadence",
            this->adaptiveConsecutiveProbeFailures,
            this->adaptiveRearmFallbackLimit
        );
        this->adaptiveRearmRequired = false;
        this->adaptiveRearmNotBefore.reset();
        this->adaptiveStableRearmSince.reset();
        this->adaptiveRearmFallbackLimit = 0;
        this->adaptiveNextRampAt.reset();
    }

    // A validated constant cadence already supplies the desired smoothness.
    // Do not probe a higher generated-frame level until it becomes unsuitable.
    if (this->adaptiveStableCadenceLimit)
        return;

    if (this->adaptiveRampEvaluationAt) {
        if (now < *this->adaptiveRampEvaluationAt)
            return;

        const size_t testedLimit = this->adaptiveGenerationLimit;
        if (this->adaptiveBridgeActive) {
            const double targetFps = static_cast<double>(this->profile.target_fps);
            const double baselineOutputFps = std::min(
                targetFps,
                this->adaptiveBridgeBaselineBaseFps *
                    static_cast<double>(this->adaptiveBridgeBaselineLimit + 1)
            );
            const double currentOutputFps = std::min(
                targetFps,
                baseFps * static_cast<double>(testedLimit + 1)
            );
            const bool accepted =
                baseFps >= adaptiveMinimumBaseFps &&
                baseFps >= this->adaptiveBridgeBaselineBaseFps *
                    adaptiveBridgeMinimumBaseRetention &&
                currentOutputFps >= baselineOutputFps * adaptiveRampMarginalGain;
            logAdaptiveBridgeResult(
                accepted,
                this->adaptiveBridgeBaselineLimit,
                testedLimit,
                this->adaptiveBridgeBaselineBaseFps,
                baseFps,
                baselineOutputFps,
                currentOutputFps
            );

            this->adaptiveRampEvaluationAt.reset();
            this->adaptiveBridgeActive = false;
            this->adaptiveOutputCredit = 0.0;
            if (!accepted) {
                this->adaptiveGenerationLimit = this->adaptiveBridgeBaselineLimit;
                this->scheduleAdaptiveRearm(now, "bridge-rejected");
                return;
            }

            this->adaptiveConsecutiveProbeFailures = 0;
            this->adaptiveLastFailedRampLimit = 0;
            this->adaptiveConsecutiveRampFailures = 0;
            this->adaptiveFailedRampBaselineBaseFps = 0.0;
            this->adaptiveNextRampAt = now + adaptiveRampStepDelay;
            if (this->profile.adaptive_stable_cadence) {
                this->adaptiveStableCadenceRetryAt =
                    now + adaptiveStableCadenceStrictSettlingDuration;
            }
            return;
        }

        const double previousOutputFps = std::min(
            static_cast<double>(this->profile.target_fps),
            this->adaptiveRampBaselineBaseFps *
                static_cast<double>(this->adaptiveRampPreviousLimit + 1)
        );
        const double currentOutputFps = std::min(
            static_cast<double>(this->profile.target_fps),
            baseFps * static_cast<double>(testedLimit + 1)
        );
        const bool throughputRegressed =
            currentOutputFps < previousOutputFps * adaptiveRampThroughputTolerance;
        const bool baseCollapsedForMarginalGain =
            baseFps < this->adaptiveRampBaselineBaseFps * adaptiveRampBaseCollapseRatio &&
            currentOutputFps < previousOutputFps * adaptiveRampMarginalGain;
        const bool accepted = !throughputRegressed && !baseCollapsedForMarginalGain;
        const size_t bridgeLimit = std::min(configuredLimit, testedLimit + 1);
        const bool canBridge =
            !accepted &&
            this->adaptiveRampPreviousLimit == 0 &&
            testedLimit == 1 &&
            bridgeLimit >= 2 &&
            baseFps >= adaptiveMinimumBaseFps &&
            currentOutputFps >=
                previousOutputFps * adaptiveBridgeMinimumOutputRetention &&
            previousOutputFps <
                static_cast<double>(this->profile.target_fps) *
                    adaptiveBridgeTargetDeficitRatio;
        if (canBridge) {
            logAdaptiveBridge(
                this->adaptiveRampPreviousLimit,
                testedLimit,
                bridgeLimit,
                this->adaptiveRampBaselineBaseFps,
                baseFps,
                previousOutputFps,
                currentOutputFps
            );
            this->adaptiveBridgeActive = true;
            this->adaptiveBridgeBaselineLimit = this->adaptiveRampPreviousLimit;
            this->adaptiveBridgeBaselineBaseFps =
                this->adaptiveRampBaselineBaseFps;
            this->adaptiveGenerationLimit = bridgeLimit;
            this->adaptiveRampEvaluationAt = now + adaptiveRampEvaluationDuration;
            this->adaptiveOutputCredit = 0.0;
            return;
        }

        logAdaptiveRampResult(
            accepted,
            this->adaptiveRampPreviousLimit,
            testedLimit,
            this->adaptiveRampBaselineBaseFps,
            baseFps,
            previousOutputFps,
            currentOutputFps
        );

        this->adaptiveRampEvaluationAt.reset();
        this->adaptiveOutputCredit = 0.0;
        if (!accepted) {
            this->adaptiveGenerationLimit = this->adaptiveRampPreviousLimit;
            if (this->adaptiveRampPreviousLimit == 0) {
                this->scheduleAdaptiveRearm(now, "ramp-rejected");
            } else {
                if (this->adaptiveLastFailedRampLimit != testedLimit) {
                    this->adaptiveLastFailedRampLimit = testedLimit;
                    this->adaptiveConsecutiveRampFailures = 0;
                }
                this->adaptiveConsecutiveRampFailures++;
                this->adaptiveFailedRampBaselineBaseFps =
                    this->adaptiveRampBaselineBaseFps;
                const auto retryDelay = adaptiveRampRetryDelayForFailures(
                    this->adaptiveConsecutiveRampFailures
                );
                this->adaptiveNextRampAt = now + retryDelay;
                logAdaptiveRampBackoff(
                    testedLimit,
                    this->adaptiveConsecutiveRampFailures,
                    this->adaptiveFailedRampBaselineBaseFps,
                    retryDelay
                );
            }
            return;
        }
        this->adaptiveConsecutiveProbeFailures = 0;
        this->adaptiveLastFailedRampLimit = 0;
        this->adaptiveConsecutiveRampFailures = 0;
        this->adaptiveFailedRampBaselineBaseFps = 0.0;
        this->adaptiveNextRampAt = now + adaptiveRampStepDelay;
        if (this->profile.adaptive_stable_cadence) {
            this->adaptiveStableCadenceRetryAt =
                now + adaptiveStableCadenceStrictSettlingDuration;
        }
    }

    if (this->adaptiveGenerationLimit >= configuredLimit)
        return;
    if (this->adaptiveNextRampAt && now < *this->adaptiveNextRampAt) {
        const size_t nextLimit = this->adaptiveGenerationLimit + 1;
        const bool failedRampRecovered =
            this->adaptiveConsecutiveRampFailures > 0 &&
            this->adaptiveLastFailedRampLimit == nextLimit &&
            this->adaptiveFailedRampBaselineBaseFps > 0.0 &&
            baseFps >= this->adaptiveFailedRampBaselineBaseFps *
                adaptiveRampEarlyRetryBaseImprovement;
        if (!failedRampRecovered)
            return;

        logAdaptiveRampEarlyRetry(
            nextLimit,
            this->adaptiveFailedRampBaselineBaseFps,
            baseFps
        );
        this->adaptiveNextRampAt.reset();
    }

    this->adaptiveRampPreviousLimit = this->adaptiveGenerationLimit;
    this->adaptiveRampBaselineBaseFps = baseFps;
    this->adaptiveGenerationLimit++;
    this->adaptiveRampEvaluationAt = now + adaptiveRampEvaluationDuration;
    this->adaptiveOutputCredit = 0.0;
    logAdaptiveRamp(
        this->adaptiveRampPreviousLimit,
        this->adaptiveGenerationLimit,
        baseFps
    );
}

VkResult Swapchain::present(const vk::Vulkan& vk,
        VkQueue queue, VkSwapchainKHR swapchain,
        void* next_chain, uint32_t imageIdx,
        const std::vector<VkSemaphore>& semaphores) {
    if (this->swapchainRecreationRequested) {
        logSwapchainRecreation(this->fidx, this->idx, "pending");
        return VK_ERROR_OUT_OF_DATE_KHR;
    }

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
    bool requestSwapchainRecreation = false;

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
            const bool discontinuityRecoveryActive =
                this->profile.adaptive &&
                this->adaptiveDiscontinuityRecoveryDeadline.has_value();
            const size_t recoveryGenerationLimit = discontinuityRecoveryActive
                ? this->adaptiveDiscontinuityGenerationLimit
                : this->validatedAdaptiveGenerationLimit();
            const bool useSoftDiscontinuityRecovery =
                discontinuityRecoveryActive &&
                !this->adaptiveDiscontinuitySoftRecoveryAttempted;
            if (useSoftDiscontinuityRecovery) {
                this->adaptiveDiscontinuitySoftRecoveryAttempted = true;
                logAdaptiveDiscontinuitySoftRecovery(recoveryGenerationLimit);
            }
            if (this->profile.adaptive && presentRecoveryRecreateEnabled() &&
                    !useSoftDiscontinuityRecovery) {
                const auto recoveryNow = DiagnosticsClock::now();
                if (!this->adaptiveRecoveryState ||
                        !this->adaptiveRecoveryState->lastSwapchainRecreation ||
                        recoveryNow -
                            *this->adaptiveRecoveryState->lastSwapchainRecreation >=
                            adaptiveRecreationCooldown) {
                    requestSwapchainRecreation = true;
                    if (this->adaptiveRecoveryState) {
                        this->adaptiveRecoveryState->lastSwapchainRecreation = recoveryNow;
                        this->adaptiveRecoveryState->nextContextIsRecovery = true;
                        this->adaptiveRecoveryState->nextContextGenerationLimit =
                            recoveryGenerationLimit;
                        this->adaptiveRecoveryState->nextContextIsDiscontinuityRecovery =
                            discontinuityRecoveryActive;
                        this->adaptiveRecoveryState->
                            nextContextDiscontinuityBaselineBaseFps =
                                this->adaptiveDiscontinuityBaselineBaseFps;
                        this->adaptiveRecoveryState->nextContextDiscontinuityDeadline =
                            this->adaptiveDiscontinuityRecoveryDeadline;
                        this->adaptiveRecoveryState->
                            nextContextDiscontinuitySoftRecoveryAttempted =
                                this->adaptiveDiscontinuitySoftRecoveryAttempted;
                    }
                } else {
                    const auto elapsed = recoveryNow -
                        *this->adaptiveRecoveryState->lastSwapchainRecreation;
                    logSwapchainRecreationSuppressed(
                        std::chrono::duration<double, std::milli>(
                            adaptiveRecreationCooldown - elapsed
                        ).count()
                    );
                }
            }
            const size_t recoveryWarmupFrames = this->profile.adaptive &&
                    !requestSwapchainRecreation
                ? adaptiveHistoryWarmupFrames
                : 0;
            logPresentRecovery(
                this->fidx, this->idx, 0, recoveryImageIndex,
                this->generatedImageAcquireBypassCount,
                boundedRecoveryProbe ? "bounded-retry" : "nonblocking-retry",
                recoveryWarmupFrames,
                requestSwapchainRecreation
            );
            this->generatedImageAcquireBypassCount = 0;
            if (this->profile.adaptive && !requestSwapchainRecreation) {
                const auto recoveryNow = DiagnosticsClock::now();
                this->beginAdaptiveStabilization(
                    recoveryNow, "generated-image-recovery"
                );
                if (!discontinuityRecoveryActive) {
                    this->restoreAdaptiveGenerationLimit(
                        recoveryNow,
                        recoveryGenerationLimit,
                        "generated-image-recovery"
                    );
                }
            } else {
                this->resetAdaptiveScheduler(DiagnosticsClock::now());
            }
            if (recoveryWarmupFrames || requestSwapchainRecreation) {
                // The successful probe owns a swapchain image. Copy the real
                // image into it and present it below before either warming the
                // current context or asking the game to recreate that context.
                recoveryWarmupImage = recoveryImageIndex;
                this->adaptiveHistoryWarmupRemaining = recoveryWarmupFrames;
                this->adaptiveHistoryWarmupIsRecovery = recoveryWarmupFrames > 0;
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

        if (requestSwapchainRecreation) {
            this->swapchainRecreationRequested = true;
            logSwapchainRecreation(this->fidx, this->idx, "adaptive-recovery");
            logSlowPresentOperation(
                "present-total", this->fidx, this->idx,
                presentStarted, VK_ERROR_OUT_OF_DATE_KHR
            );
            this->fidx++;
            return VK_ERROR_OUT_OF_DATE_KHR;
        }

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
