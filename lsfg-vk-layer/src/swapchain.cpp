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
#include <atomic>
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

#include <unistd.h>

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
    // Steam/GameScope and some DX12 presentation paths can briefly submit a
    // burst of images during an overlay, focus or display-mode transition.
    // Those intervals are not useful gameplay cadence samples: accepting one
    // would drag the smoothed base rate into the hundreds of FPS, causing the
    // next ordinary frame to look like a false cadence drop.
    constexpr double adaptiveTransientFastBurstCadenceRatio = 3.0;
    constexpr double adaptiveTransientFastBurstTargetRatio = 2.0;
    constexpr double adaptiveRampThroughputTolerance = 0.95;
    constexpr double adaptiveRampBaseCollapseRatio = 0.70;
    constexpr double adaptiveRampMarginalGain = 1.15;
    constexpr double adaptiveRampTargetSatisfiedRatio = 0.95;
    constexpr double adaptiveStrictLoadCollapseRatio = 0.80;
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
    constexpr auto adaptiveTargetDeficitDuration = std::chrono::seconds(1);
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
    constexpr auto adaptiveStrictLoadCollapseDuration = std::chrono::seconds(1);
    // Overlay/focus discontinuities bypass the ordinary collapse detector
    // because the raw cadence stalls before a smoothed sample can be scored.
    // Keep the last proven level, wait for real-only cadence to recover, and
    // restore it only after a bounded stable measurement.
    constexpr double adaptiveDiscontinuityRecoveredBaseRatio = 0.90;
    constexpr auto adaptiveDiscontinuityStableDuration = std::chrono::seconds(1);
    constexpr auto adaptiveDiscontinuityMaximumDuration = std::chrono::seconds(5);
    // A brief gameplay hitch does not need the full menu/focus recovery when
    // Adaptive has already proven its only available 2x generation level.
    // Refresh temporal history and resume that level; longer interruptions
    // retain the guarded discontinuity path below.
    constexpr auto adaptiveTwoXGameplayHitchMaximumDuration =
        std::chrono::milliseconds(250);
    constexpr auto adaptiveFailedProbeCooldown = std::chrono::seconds(15);
    constexpr auto adaptiveInterruptedProbeCooldown = std::chrono::seconds(2);
    constexpr auto adaptiveStableRearmDuration = std::chrono::seconds(2);
    constexpr auto adaptiveRecreationCooldown = std::chrono::seconds(5);
    constexpr auto adaptiveFastBurstDiagnosticInterval = std::chrono::seconds(1);

    std::atomic<uint32_t> nextDiagnosticsContextSequence{1};
    thread_local uint64_t activeDiagnosticsContextId{0};

    uint64_t allocateDiagnosticsContextId() {
        const auto processId = static_cast<uint64_t>(
            static_cast<uint32_t>(::getpid())
        );
        const auto sequence = static_cast<uint64_t>(
            nextDiagnosticsContextSequence.fetch_add(1, std::memory_order_relaxed)
        );
        return (processId << 32) | sequence;
    }

    class DiagnosticsContextScope {
    public:
        explicit DiagnosticsContextScope(const uint64_t contextId) :
                previousContextId(activeDiagnosticsContextId) {
            activeDiagnosticsContextId = contextId;
        }

        ~DiagnosticsContextScope() {
            activeDiagnosticsContextId = this->previousContextId;
        }

        DiagnosticsContextScope(const DiagnosticsContextScope&) = delete;
        DiagnosticsContextScope& operator=(const DiagnosticsContextScope&) = delete;
    private:
        uint64_t previousContextId;
    };

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
                << " context=" << activeDiagnosticsContextId
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
                  << " context=" << activeDiagnosticsContextId
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
                  << " context=" << activeDiagnosticsContextId
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
                  << " context=" << activeDiagnosticsContextId
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
                  << " context=" << activeDiagnosticsContextId
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
                  << " context=" << activeDiagnosticsContextId
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
                  << " context=" << activeDiagnosticsContextId
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
                  << " context=" << activeDiagnosticsContextId
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
                  << " context=" << activeDiagnosticsContextId
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
                  << " context=" << activeDiagnosticsContextId
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
                  << " context=" << activeDiagnosticsContextId
                  << " reason=" << reason
                  << " tested_generated_limit=" << testedLimit << '\n';
    }

    void logAdaptiveRearm(std::string_view operation, std::string_view reason,
            size_t failures, size_t fallbackLimit,
            const DiagnosticsClock::duration cooldown,
            double baselineBaseFps, double currentBaseFps = 0.0,
            std::string_view decision = {}) {
        if (!presentDiagnosticsEnabled())
            return;

        std::cerr << "lsfg-vk: present diagnostics: operation=" << operation
                  << " context=" << activeDiagnosticsContextId
                  << " reason=" << reason
                  << " consecutive_failures=" << failures;
        if (operation == "adaptive-rearm-scheduled") {
            std::cerr << " cooldown_ms="
                      << std::chrono::duration_cast<std::chrono::milliseconds>(
                             cooldown
                         ).count()
                      << " stable_required_ms="
                      << std::chrono::duration_cast<std::chrono::milliseconds>(
                             adaptiveStableRearmDuration
                         ).count()
                      << " fallback_generated_limit=" << fallbackLimit
                      << " baseline_base_fps=" << baselineBaseFps;
        } else {
            std::cerr << " fallback_generated_limit=" << fallbackLimit
                      << " baseline_base_fps=" << baselineBaseFps
                      << " current_base_fps=" << currentBaseFps;
            if (!decision.empty())
                std::cerr << " decision=" << decision;
        }
        std::cerr << '\n';
    }

    void logAdaptiveFastCadenceBurst(double baselineBaseFps,
            double instantaneousBaseFps, double thresholdFps,
            size_t ignoredFrames, size_t totalIgnoredFrames,
            const DiagnosticsClock::duration duration) {
        if (!presentDiagnosticsEnabled())
            return;

        std::cerr << "lsfg-vk: present diagnostics: operation="
                  << "adaptive-fast-cadence-burst"
                  << " context=" << activeDiagnosticsContextId
                  << " baseline_base_fps=" << baselineBaseFps
                  << " instantaneous_base_fps=" << instantaneousBaseFps
                  << " threshold_fps=" << thresholdFps
                  << " ignored_frames=" << ignoredFrames
                  << " total_ignored_frames=" << totalIgnoredFrames
                  << " duration_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         duration
                     ).count()
                  << '\n';
    }

    void logAdaptiveFastCadenceBurstComplete(size_t totalIgnoredFrames,
            const DiagnosticsClock::duration duration) {
        if (!presentDiagnosticsEnabled())
            return;

        std::cerr << "lsfg-vk: present diagnostics: operation="
                  << "adaptive-fast-cadence-burst-complete"
                  << " context=" << activeDiagnosticsContextId
                  << " total_ignored_frames=" << totalIgnoredFrames
                  << " duration_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         duration
                     ).count()
                  << '\n';
    }

    void logAdaptiveRampBackoff(size_t testedLimit, size_t failures,
            double baselineBaseFps,
            const std::chrono::steady_clock::duration delay) {
        if (!presentDiagnosticsEnabled())
            return;

        std::cerr << "lsfg-vk: present diagnostics: operation=adaptive-ramp-backoff"
                  << " context=" << activeDiagnosticsContextId
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
                  << " context=" << activeDiagnosticsContextId
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
                  << " context=" << activeDiagnosticsContextId
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
                  << " context=" << activeDiagnosticsContextId
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
            double projectedOutputFps,
            std::string_view reason = "stable-cadence-collapse") {
        if (!presentDiagnosticsEnabled())
            return;

        std::cerr << "lsfg-vk: present diagnostics: operation=adaptive-rescue-start"
                  << " context=" << activeDiagnosticsContextId
                  << " generated_limit=" << generatedLimit
                  << " baseline_base_fps=" << baselineBaseFps
                  << " current_base_fps=" << currentBaseFps
                  << " projected_output_fps=" << projectedOutputFps
                  << " reason=" << reason
                  << " measurement_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         adaptiveRescueMeasurementDuration
                     ).count()
                  << '\n';
    }

    void logAdaptiveRescueComplete(size_t previousLimit, size_t resumedLimit,
            size_t requestedLimit, size_t configuredLimit,
            double baselineBaseFps, double measuredBaseFps,
            std::string_view decision) {
        if (!presentDiagnosticsEnabled())
            return;

        std::cerr << "lsfg-vk: present diagnostics: operation=adaptive-rescue-complete"
                  << " context=" << activeDiagnosticsContextId
                  << " previous_generated_limit=" << previousLimit
                  << " resumed_generated_limit=" << resumedLimit
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
                  << " context=" << activeDiagnosticsContextId
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
                  << " context=" << activeDiagnosticsContextId
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
                  << " context=" << activeDiagnosticsContextId
                  << " generated_limit=" << generationLimit
                  << " action=history-warmup"
                  << '\n';
    }

    void logAdaptiveTwoXGameplayHitchRecovery(size_t generationLimit,
            double baselineBaseFps,
            const std::chrono::steady_clock::duration rawInterval) {
        if (!presentDiagnosticsEnabled())
            return;

        std::cerr << "lsfg-vk: present diagnostics: operation="
                  << "adaptive-gameplay-hitch-recovery"
                  << " context=" << activeDiagnosticsContextId
                  << " generated_limit=" << generationLimit
                  << " baseline_base_fps=" << baselineBaseFps
                  << " raw_interval_ms="
                  << std::chrono::duration<double, std::milli>(
                         rawInterval
                     ).count()
                  << " history_warmup_frames=" << adaptiveHistoryWarmupFrames
                  << '\n';
    }

    void logSwapchainRecreationSuppressed(double remainingMs) {
        if (!presentDiagnosticsEnabled())
            return;

        std::cerr << "lsfg-vk: present diagnostics: operation=swapchain-recreation-suppressed"
                  << " context=" << activeDiagnosticsContextId
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

namespace {

    /// force every present mode in the chain to FIFO
    ///
    /// The swapchain is always created as FIFO when pacing is off, so a game asking for a
    /// different mode at present time has to be brought back in line.
    ///
    /// @param next_chain next chain pointer from the present info (WARN: shared!)
    void forceFifoPresentModes(void* next_chain) {
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

}

void layer::context_ModifySwapchainCreateInfo(const ls::GameConf& profile, uint32_t maxImages,
        VkSwapchainCreateInfoKHR& createInfo) {
    createInfo.imageUsage |=
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    switch (profile.pacing) {
        case ls::Pacing::None:
            // Reserve the selected Fixed or Adaptive capacity even while live
            // generation is off. The game owns this swapchain, so retaining
            // its capacity is what lets configuration reload turn generation
            // back on without forcing a game restart or swapchain rebuild.
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
    this->diagnosticsContextId = allocateDiagnosticsContextId();
    const DiagnosticsContextScope diagnosticsContext(
        this->diagnosticsContextId
    );

    if (this->profile.adaptive)
        this->adaptiveHistoryWarmupRemaining = adaptiveHistoryWarmupFrames;

    const VkExtent2D extent = this->info.extent;
    const bool hdr = this->info.format > 57;

    // Live-off is passthrough. Keep the backend instance loaded so the watched
    // configuration can turn the selected Fixed or Adaptive mode back on, but
    // allocate no interpolation images and open no per-swapchain model context.
    if (!this->profile.frame_generation_enabled) {
        std::cerr << "lsfg-vk: frame generation is off for this swapchain\n";
        return;
    }

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
        std::cerr << "lsfg-vk: present diagnostics enabled; context="
                  << activeDiagnosticsContextId
                  << "; slow operation threshold is "
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

    const auto rawInterval = now - *this->adaptiveLastRealFrame;
    const double rawIntervalSeconds = std::chrono::duration<double>(
        rawInterval
    ).count();
    this->adaptiveLastRealFrame = now;

    const auto finishFastCadenceBurst = [&] {
        if (!this->adaptiveFastBurstStartedAt)
            return;

        logAdaptiveFastCadenceBurstComplete(
            this->adaptiveFastBurstFrames,
            now - *this->adaptiveFastBurstStartedAt
        );
        this->adaptiveFastBurstStartedAt.reset();
        this->adaptiveLastFastBurstDiagnostic.reset();
        this->adaptiveFastBurstFrames = 0;
        this->adaptiveFastBurstFramesSinceDiagnostic = 0;
    };

    // Loading screens, suspension and base rates below 10 FPS do not provide
    // useful motion history. Present real frames until cadence has been stable
    // for a bounded interval instead of immediately reapplying model load.
    if (rawIntervalSeconds <= 0.0) {
        finishFastCadenceBurst();
        this->beginAdaptiveStabilization(now, "cadence-stall");
        return {};
    }

    const double baselineBaseFps = this->adaptiveSmoothedIntervalSeconds > 0.0
        ? 1.0 / this->adaptiveSmoothedIntervalSeconds
        : 0.0;
    const double instantaneousBaseFps = 1.0 / rawIntervalSeconds;
    const double fastBurstThresholdFps = std::max(
        baselineBaseFps * adaptiveTransientFastBurstCadenceRatio,
        static_cast<double>(this->profile.target_fps) *
            adaptiveTransientFastBurstTargetRatio
    );
    if (instantaneousBaseFps > fastBurstThresholdFps) {
        // Do not manufacture generated work for a transient that is already
        // faster than the requested output. Preserve the proven gameplay
        // baseline and pause evaluation windows which require real generated
        // workload instead of letting wall-clock time validate an untested
        // multiplier while DX12 is submitting the burst.
        if (!this->adaptiveFastBurstStartedAt)
            this->adaptiveFastBurstStartedAt = now;
        this->adaptiveFastBurstFrames++;
        this->adaptiveFastBurstFramesSinceDiagnostic++;

        const auto pauseEvaluation = [&rawInterval](auto& deadline) {
            if (deadline)
                *deadline += rawInterval;
        };
        pauseEvaluation(this->adaptiveStabilizationUntil);
        pauseEvaluation(this->adaptiveRampEvaluationAt);
        pauseEvaluation(this->adaptiveStableCadenceEvaluationAt);
        pauseEvaluation(this->adaptiveRescueUntil);

        this->adaptiveOutputCredit = 0.0;
        this->adaptiveCadenceDropFrames = 0;
        this->adaptiveTargetDeficitSince.reset();
        this->adaptiveStableRearmSince.reset();
        this->adaptiveRearmImprovementSince.reset();
        this->adaptiveStrictLoadCollapseSince.reset();
        this->adaptiveStableCadenceOutsideRangeSince.reset();
        this->adaptiveDiscontinuityStableSince.reset();

        if (!this->adaptiveLastFastBurstDiagnostic ||
                now - *this->adaptiveLastFastBurstDiagnostic >=
                    adaptiveFastBurstDiagnosticInterval) {
            logAdaptiveFastCadenceBurst(
                baselineBaseFps,
                instantaneousBaseFps,
                fastBurstThresholdFps,
                this->adaptiveFastBurstFramesSinceDiagnostic,
                this->adaptiveFastBurstFrames,
                now - *this->adaptiveFastBurstStartedAt
            );
            this->adaptiveLastFastBurstDiagnostic = now;
            this->adaptiveFastBurstFramesSinceDiagnostic = 0;
        }
        return {};
    }
    finishFastCadenceBurst();

    if (rawIntervalSeconds > 1.0 / adaptiveMinimumBaseFps) {
        const size_t configuredGenerationLimit = std::min(
            this->destinationImages.size(),
            this->profile.adaptive_max_multiplier - 1
        );
        const size_t validatedGenerationLimit =
            this->validatedAdaptiveGenerationLimit();
        const bool shortTwoXGameplayHitch =
            configuredGenerationLimit == 1 &&
            validatedGenerationLimit == 1 &&
            rawInterval <= adaptiveTwoXGameplayHitchMaximumDuration;
        if (shortTwoXGameplayHitch) {
            // Keep the proven 2x policy, but feed the model fresh real-frame
            // history before generating again. If Gamescope is actually
            // withholding generated images, the existing bounded acquire and
            // swapchain recovery path will still take over after this warmup.
            this->adaptiveHistoryWarmupRemaining =
                adaptiveHistoryWarmupFrames;
            this->adaptiveHistoryWarmupIsRecovery = true;
            logAdaptiveTwoXGameplayHitchRecovery(
                validatedGenerationLimit,
                baselineBaseFps,
                rawInterval
            );
            this->resetAdaptiveScheduler(now);
            return {};
        }

        this->beginAdaptiveStabilization(now, "cadence-stall");
        return {};
    }

    // A sustained interval jump can be a menu/focus transition, but it can
    // also be a legitimate heavier gameplay scene. Three samples avoid
    // treating an isolated hitch as a cadence change. Unlike a hard stall,
    // this path performs only the ordinary one-second stabilization and then
    // rebases Adaptive at the new measured rate.
    const bool cadenceDropCandidate =
        this->adaptiveSmoothedIntervalSeconds > 0.0 &&
            rawIntervalSeconds >=
                this->adaptiveSmoothedIntervalSeconds * adaptiveCadenceDropRatio;
    if (cadenceDropCandidate) {
        this->adaptiveStableRearmSince.reset();
        this->adaptiveRearmImprovementSince.reset();
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
                this->adaptiveTargetDeficitSince.reset();
                this->adaptiveBridgeActive = false;
                this->adaptiveBridgeBaselineLimit = 0;
                this->adaptiveBridgeBaselineBaseFps = 0.0;
                this->adaptiveRearmRequired = false;
                this->adaptiveRearmNotBefore.reset();
                this->adaptiveStableRearmSince.reset();
                this->adaptiveRearmImprovementSince.reset();
                this->adaptiveRearmReason.clear();
                this->adaptiveRearmBaselineBaseFps = 0.0;
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
                      << " context=" << activeDiagnosticsContextId
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
                          << " context=" << activeDiagnosticsContextId
                          << " base_fps=" << baseFps
                          << " target_fps=" << this->profile.target_fps
                          << " generated=0 max_generated=0"
                          << " phase="
                          << (this->adaptiveRescueFromStrictLoad
                                  ? "strict-load-rescue"
                                  : "rescue")
                          << '\n';
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
        const bool strictLoadRescue = this->adaptiveRescueFromStrictLoad;
        const size_t strictLoadLimit = std::min(
            this->adaptiveRescueStrictLoadLimit, configuredLimit
        );

        std::string_view decision = "resume-strict";
        this->adaptiveGenerationLimit = previousLimit;
        this->adaptiveRampEvaluationAt.reset();
        this->adaptiveBridgeActive = false;
        this->adaptiveBridgeBaselineLimit = 0;
        this->adaptiveBridgeBaselineBaseFps = 0.0;
        this->adaptiveNextRampAt.reset();
        if (strictLoadRescue) {
            // A real-only measurement distinguishes inference pressure from a
            // genuinely heavier game scene. Restore the lower proven level
            // only when cadence recovers without generated-frame work;
            // otherwise retain the higher level that the scene still needs.
            this->adaptiveNextRampAt = this->adaptiveRescueCooldownUntil;
            if (baseRecovered) {
                decision = "strict-load-restored";
            } else {
                this->adaptiveGenerationLimit = std::max(
                    previousLimit, strictLoadLimit
                );
                decision = "strict-load-retained";
            }
        } else if (baseRecovered) {
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
        this->adaptiveRescueFromStrictLoad = false;
        this->adaptiveRescueStrictLoadLimit = 0;
        this->adaptiveTargetDeficitSince.reset();
        this->adaptiveOutputCredit = 0.0;
        if (this->adaptiveRescueCooldownUntil)
            this->adaptiveStableCadenceRetryAt = this->adaptiveRescueCooldownUntil;
        logAdaptiveRescueComplete(
            previousLimit,
            this->adaptiveGenerationLimit,
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
                      << " context=" << activeDiagnosticsContextId
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
                    this->adaptiveTargetDeficitSince.reset();
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

    // A ramp can pass its one-second evaluation and still settle into a
    // slower compositor divisor afterwards. Monitor only when strict Adaptive
    // is actually using the newly validated maximum load. If that load causes
    // a sustained base-rate collapse without a meaningful estimated-output
    // gain, briefly measure real-only cadence and return to the previous proven
    // level instead of remaining trapped at the higher multiplier.
    const double strictBaselineOutputFps = std::min(
        static_cast<double>(this->profile.target_fps),
        this->adaptiveStrictLoadBaselineBaseFps *
            static_cast<double>(this->adaptiveStrictLoadBaselineLimit + 1)
    );
    const double strictCurrentOutputFps = std::min(
        static_cast<double>(this->profile.target_fps),
        baseFps * static_cast<double>(this->adaptiveGenerationLimit + 1)
    );
    const bool strictLoadCollapse =
        !this->adaptiveStableCadenceLimit &&
        !this->adaptiveRampEvaluationAt &&
        !this->adaptiveRearmRequired &&
        !this->adaptiveRescueUntil &&
        this->adaptiveStrictLoadBaselineBaseFps > 0.0 &&
        this->adaptiveGenerationLimit > this->adaptiveStrictLoadBaselineLimit &&
        generatedFrameCount == this->adaptiveGenerationLimit &&
        baseFps < this->adaptiveStrictLoadBaselineBaseFps *
            adaptiveStrictLoadCollapseRatio &&
        strictCurrentOutputFps < strictBaselineOutputFps *
            adaptiveRampMarginalGain &&
        (!this->adaptiveRescueCooldownUntil ||
         now >= *this->adaptiveRescueCooldownUntil);
    if (strictLoadCollapse) {
        if (!this->adaptiveStrictLoadCollapseSince)
            this->adaptiveStrictLoadCollapseSince = now;
        if (now - *this->adaptiveStrictLoadCollapseSince >=
                adaptiveStrictLoadCollapseDuration) {
            const size_t collapsedLimit = this->adaptiveGenerationLimit;
            this->adaptiveRescuePreviousLimit =
                this->adaptiveStrictLoadBaselineLimit;
            this->adaptiveRescueBaselineBaseFps =
                this->adaptiveStrictLoadBaselineBaseFps;
            this->adaptiveRescueFromStrictLoad = true;
            this->adaptiveRescueStrictLoadLimit = collapsedLimit;
            this->adaptiveRescueUntil = now + adaptiveRescueMeasurementDuration;
            this->adaptiveRescueCooldownUntil = now + adaptiveRescueCooldown;
            this->adaptiveTargetDeficitSince.reset();
            this->adaptiveStrictLoadBaselineLimit = 0;
            this->adaptiveStrictLoadBaselineBaseFps = 0.0;
            this->adaptiveStrictLoadCollapseSince.reset();
            this->adaptiveOutputCredit = 0.0;
            logAdaptiveRescueStart(
                collapsedLimit,
                this->adaptiveRescueBaselineBaseFps,
                baseFps,
                strictCurrentOutputFps,
                "strict-load-collapse"
            );
            return {};
        }
    } else {
        this->adaptiveStrictLoadCollapseSince.reset();
    }

    if (presentDiagnosticsEnabled() &&
            (!this->adaptiveLastDiagnostic ||
             now - *this->adaptiveLastDiagnostic >= std::chrono::seconds(1))) {
        this->adaptiveLastDiagnostic = now;
        const auto rearmRemaining = this->adaptiveRearmRequired &&
                this->adaptiveRearmNotBefore &&
                now < *this->adaptiveRearmNotBefore
            ? *this->adaptiveRearmNotBefore - now
            : DiagnosticsClock::duration::zero();
        std::cerr << "lsfg-vk: present diagnostics: operation=adaptive-plan"
                  << " context=" << activeDiagnosticsContextId
                  << " base_fps=" << baseFps
                  << " target_fps=" << this->profile.target_fps
                  << " generated=" << generatedFrameCount
                  << " max_generated=" << maximumGeneratedFrameCount
                  << " configured_max_generated="
                  << this->profile.adaptive_max_multiplier - 1;
        if (this->adaptiveRearmRequired) {
            std::cerr << " phase=rearm-cooldown"
                      << " rearm_reason=" << this->adaptiveRearmReason
                      << " cooldown_remaining_ms="
                      << std::chrono::duration_cast<std::chrono::milliseconds>(
                             rearmRemaining
                         ).count()
                      << " rearm_baseline_base_fps="
                      << this->adaptiveRearmBaselineBaseFps;
        }
        std::cerr << '\n';
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
    if (this->adaptiveFastBurstStartedAt) {
        logAdaptiveFastCadenceBurstComplete(
            this->adaptiveFastBurstFrames,
            now - *this->adaptiveFastBurstStartedAt
        );
    }
    this->adaptiveFastBurstStartedAt.reset();
    this->adaptiveLastFastBurstDiagnostic.reset();
    this->adaptiveFastBurstFrames = 0;
    this->adaptiveFastBurstFramesSinceDiagnostic = 0;
    this->adaptiveTargetDeficitSince.reset();
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
    this->adaptiveTargetDeficitSince.reset();
    this->adaptiveRampBaselineBaseFps = 0.0;
    this->adaptiveBridgeActive = false;
    this->adaptiveBridgeBaselineLimit = 0;
    this->adaptiveBridgeBaselineBaseFps = 0.0;
    this->adaptiveRearmRequired = false;
    this->adaptiveRearmNotBefore.reset();
    this->adaptiveStableRearmSince.reset();
    this->adaptiveRearmImprovementSince.reset();
    this->adaptiveRearmReason.clear();
    this->adaptiveRearmBaselineBaseFps = 0.0;
    this->adaptiveRearmFallbackLimit = 0;
    this->adaptiveConsecutiveProbeFailures = 0;
    this->adaptiveLastFailedRampLimit = 0;
    this->adaptiveConsecutiveRampFailures = 0;
    this->adaptiveFailedRampBaselineBaseFps = 0.0;
    this->adaptiveStrictLoadBaselineLimit = 0;
    this->adaptiveStrictLoadBaselineBaseFps = 0.0;
    this->adaptiveStrictLoadCollapseSince.reset();
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
    this->adaptiveTargetDeficitSince.reset();
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
        const size_t fallbackLimit,
        const double baselineBaseFps) {
    const bool interrupted = reason == "probe-interrupted";
    const auto cooldown = interrupted
        ? adaptiveInterruptedProbeCooldown
        : adaptiveFailedProbeCooldown;
    if (!interrupted)
        this->adaptiveConsecutiveProbeFailures++;
    this->adaptiveRearmRequired = true;
    this->adaptiveRearmNotBefore = now + cooldown;
    this->adaptiveStableRearmSince.reset();
    this->adaptiveRearmImprovementSince.reset();
    this->adaptiveRearmReason = reason;
    this->adaptiveRearmBaselineBaseFps = baselineBaseFps;
    this->adaptiveRearmFallbackLimit = fallbackLimit;
    this->adaptiveTargetDeficitSince.reset();
    this->adaptiveNextRampAt = this->adaptiveRearmNotBefore;
    logAdaptiveRearm(
        "adaptive-rearm-scheduled",
        reason,
        this->adaptiveConsecutiveProbeFailures,
        this->adaptiveRearmFallbackLimit,
        cooldown,
        this->adaptiveRearmBaselineBaseFps
    );
}

void Swapchain::beginAdaptiveStabilization(
        const std::chrono::steady_clock::time_point now,
        const std::string_view reason) {
    if (!this->profile.adaptive)
        return;

    const bool cadenceChange =
        reason == "cadence-stall" || reason == "cadence-drop";
    const bool hardDiscontinuity = reason == "cadence-stall";
    if (cadenceChange)
        this->adaptiveDiscontinuityStableSince.reset();
    if (hardDiscontinuity &&
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
        const double rearmBaselineBaseFps = this->adaptiveBridgeActive
            ? this->adaptiveBridgeBaselineBaseFps
            : this->adaptiveRampBaselineBaseFps;
        logAdaptiveProbeAborted(reason, this->adaptiveGenerationLimit);
        this->scheduleAdaptiveRearm(
            now,
            "probe-interrupted",
            rearmFallbackLimit,
            rearmBaselineBaseFps
        );
    } else if (this->adaptiveRearmRequired) {
        // A fresh cadence disruption restarts the stable-cadence requirement,
        // but it does not extend the already bounded cooldown indefinitely.
        this->adaptiveStableRearmSince.reset();
        this->adaptiveRearmImprovementSince.reset();
    }
    // Startup can include an uncapped splash screen or launcher followed by
    // normal gameplay. Do not let those first samples start a probe that the
    // gameplay transition immediately interrupts and unnecessarily penalizes.
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
    this->adaptiveRescueFromStrictLoad = false;
    this->adaptiveRescueStrictLoadLimit = 0;
    this->adaptiveStrictLoadBaselineLimit = 0;
    this->adaptiveStrictLoadBaselineBaseFps = 0.0;
    this->adaptiveStrictLoadCollapseSince.reset();
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
        const bool interrupted =
            this->adaptiveRearmReason == "probe-interrupted";
        const bool baseImproved = !interrupted &&
            this->adaptiveRearmBaselineBaseFps > 0.0 &&
            baseFps >= this->adaptiveRearmBaselineBaseFps *
                adaptiveRampEarlyRetryBaseImprovement;
        if (baseImproved) {
            if (!this->adaptiveRearmImprovementSince)
                this->adaptiveRearmImprovementSince = now;
        } else {
            this->adaptiveRearmImprovementSince.reset();
        }
        const bool performanceRecovered =
            this->adaptiveRearmImprovementSince &&
            now - *this->adaptiveRearmImprovementSince >=
                adaptiveStableRearmDuration;
        if (!cadenceStable || (!cooldownElapsed && !performanceRecovered))
            return;

        const std::string_view decision = interrupted
            ? "interruption-settled"
            : performanceRecovered && !cooldownElapsed
                ? "performance-recovered"
                : "cooldown-elapsed";
        logAdaptiveRearm(
            "adaptive-rearm-ready",
            this->adaptiveRearmReason,
            this->adaptiveConsecutiveProbeFailures,
            this->adaptiveRearmFallbackLimit,
            DiagnosticsClock::duration::zero(),
            this->adaptiveRearmBaselineBaseFps,
            baseFps,
            decision
        );
        this->adaptiveRearmRequired = false;
        this->adaptiveRearmNotBefore.reset();
        this->adaptiveStableRearmSince.reset();
        this->adaptiveRearmImprovementSince.reset();
        this->adaptiveRearmReason.clear();
        this->adaptiveRearmBaselineBaseFps = 0.0;
        this->adaptiveRearmFallbackLimit = 0;
        this->adaptiveNextRampAt.reset();
    }

    // A validated constant cadence already supplies the desired smoothness.
    // Do not probe a higher generated-frame level until it becomes unsuitable.
    if (this->adaptiveStableCadenceLimit) {
        this->adaptiveTargetDeficitSince.reset();
        return;
    }

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
                this->scheduleAdaptiveRearm(
                    now,
                    "bridge-rejected",
                    this->adaptiveBridgeBaselineLimit,
                    this->adaptiveBridgeBaselineBaseFps
                );
                return;
            }

            this->adaptiveConsecutiveProbeFailures = 0;
            this->adaptiveStrictLoadBaselineLimit =
                this->adaptiveBridgeBaselineLimit;
            this->adaptiveStrictLoadBaselineBaseFps =
                this->adaptiveBridgeBaselineBaseFps;
            this->adaptiveStrictLoadCollapseSince.reset();
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
                this->scheduleAdaptiveRearm(
                    now,
                    "ramp-rejected",
                    this->adaptiveRampPreviousLimit,
                    this->adaptiveRampBaselineBaseFps
                );
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
        this->adaptiveStrictLoadBaselineLimit =
            this->adaptiveRampPreviousLimit;
        this->adaptiveStrictLoadBaselineBaseFps =
            this->adaptiveRampBaselineBaseFps;
        this->adaptiveStrictLoadCollapseSince.reset();
        this->adaptiveLastFailedRampLimit = 0;
        this->adaptiveConsecutiveRampFailures = 0;
        this->adaptiveFailedRampBaselineBaseFps = 0.0;
        this->adaptiveNextRampAt = now + adaptiveRampStepDelay;
        if (this->profile.adaptive_stable_cadence) {
            this->adaptiveStableCadenceRetryAt =
                now + adaptiveStableCadenceStrictSettlingDuration;
        }
    }

    // Once the current proven ceiling can already supply the requested target,
    // a higher multiplier adds inference load without useful output. Keep the
    // lower level and reconsider automatically if the measured base rate later
    // falls far enough that this capacity is no longer sufficient.
    const double validatedOutputFps = baseFps *
        static_cast<double>(this->adaptiveGenerationLimit + 1);
    const bool targetSatisfied = validatedOutputFps >=
        static_cast<double>(this->profile.target_fps) *
            adaptiveRampTargetSatisfiedRatio;
    if (targetSatisfied) {
        this->adaptiveTargetDeficitSince.reset();
        return;
    }

    if (this->adaptiveGenerationLimit >= configuredLimit) {
        this->adaptiveTargetDeficitSince.reset();
        return;
    }
    if (!this->adaptiveTargetDeficitSince) {
        this->adaptiveTargetDeficitSince = now;
        return;
    }
    if (now - *this->adaptiveTargetDeficitSince <
            adaptiveTargetDeficitDuration)
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
    this->adaptiveTargetDeficitSince.reset();
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
    const DiagnosticsContextScope diagnosticsContext(
        this->diagnosticsContextId
    );

    if (this->swapchainRecreationRequested) {
        logSwapchainRecreation(this->fidx, this->idx, "pending");
        return VK_ERROR_OUT_OF_DATE_KHR;
    }

    const auto presentStarted = startPresentDiagnostic();

    // Frame generation is live-disabled; hand the game's own image directly to
    // the driver without copies, model scheduling, fences or generated images.
    if (!this->profile.frame_generation_enabled) {
        if (this->profile.pacing == ls::Pacing::None)
            forceFifoPresentModes(next_chain);

        const VkPresentInfoKHR presentInfo{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .pNext = next_chain,
            .waitSemaphoreCount = static_cast<uint32_t>(semaphores.size()),
            .pWaitSemaphores = semaphores.data(),
            .swapchainCount = 1,
            .pSwapchains = &swapchain,
            .pImageIndices = &imageIdx,
        };
        const auto res = vk.df().QueuePresentKHR(queue, &presentInfo);
        if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
            throw ls::vulkan_error(res, "vkQueuePresentKHR() failed");

        logSlowPresentOperation("present-total", this->fidx, this->idx, presentStarted, res);
        this->fidx++;
        return res;
    }

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
    if (this->profile.pacing == ls::Pacing::None)
        forceFifoPresentModes(next_chain);

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
