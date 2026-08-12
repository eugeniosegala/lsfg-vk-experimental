/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "swapchain.hpp"
#include "adaptive_scheduler.hpp"
#include "lsfg-vk-backend/lsfgvk.hpp"
#include "lsfg-vk-common/configuration/config.hpp"
#include "lsfg-vk-common/helpers/errors.hpp"
#include "lsfg-vk-common/helpers/pointers.hpp"
#include "lsfg-vk-common/vulkan/command_buffer.hpp"
#include "lsfg-vk-common/vulkan/image.hpp"
#include "lsfg-vk-common/vulkan/semaphore.hpp"
#include "lsfg-vk-common/vulkan/vulkan.hpp"

#include <algorithm>
#include <array>
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
#include <span>
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
                             AdaptiveScheduler::stableRearmDuration()
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
                         AdaptiveScheduler::rescueMeasurementDuration()
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
                         AdaptiveScheduler::rescueCooldown()
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
                  << " history_warmup_frames="
                  << AdaptiveScheduler::historyWarmupFrameCount()
                  << '\n';
    }

    class SwapchainAdaptiveSchedulerDiagnostics final :
            public AdaptiveSchedulerDiagnostics {
    public:
        [[nodiscard]] bool enabled() const override {
            return presentDiagnosticsEnabled();
        }

        void plan(const AdaptivePlanDiagnostic& plan) override {
            if (!presentDiagnosticsEnabled())
                return;

            std::cerr << "lsfg-vk: present diagnostics: operation=adaptive-plan"
                      << " context=" << activeDiagnosticsContextId
                      << " base_fps=" << plan.baseFps
                      << " target_fps=" << plan.targetFps
                      << " generated=" << plan.generatedFrames
                      << " max_generated=" << plan.maximumGeneratedFrames;
            if (plan.configuredMaximumGeneratedFrames) {
                std::cerr << " configured_max_generated="
                          << plan.configuredMaximumGeneratedFrames;
            }
            if (!plan.phase.empty())
                std::cerr << " phase=" << plan.phase;
            if (plan.recoveryGenerationLimit)
                std::cerr << " recovery_generated_limit="
                          << plan.recoveryGenerationLimit;
            if (!plan.rearmReason.empty()) {
                std::cerr << " rearm_reason=" << plan.rearmReason
                          << " cooldown_remaining_ms="
                          << std::chrono::duration_cast<std::chrono::milliseconds>(
                                 plan.rearmCooldownRemaining
                             ).count()
                          << " rearm_baseline_base_fps="
                          << plan.rearmBaselineBaseFps;
            }
            std::cerr << '\n';
        }

        void stabilization(std::string_view reason,
                std::chrono::steady_clock::duration duration) override {
            logAdaptiveStabilization(reason, duration);
        }
        void ramp(size_t previousLimit, size_t newLimit,
                double baseFps) override {
            logAdaptiveRamp(previousLimit, newLimit, baseFps);
        }
        void rampResult(bool accepted, size_t previousLimit,
                size_t testedLimit, double previousBaseFps,
                double currentBaseFps, double previousOutputFps,
                double currentOutputFps) override {
            logAdaptiveRampResult(
                accepted, previousLimit, testedLimit, previousBaseFps,
                currentBaseFps, previousOutputFps, currentOutputFps
            );
        }
        void bridge(size_t previousLimit, size_t testedLimit,
                size_t bridgeLimit, double previousBaseFps,
                double currentBaseFps, double previousOutputFps,
                double currentOutputFps) override {
            logAdaptiveBridge(
                previousLimit, testedLimit, bridgeLimit, previousBaseFps,
                currentBaseFps, previousOutputFps, currentOutputFps
            );
        }
        void bridgeResult(bool accepted, size_t baselineLimit,
                size_t testedLimit, double baselineBaseFps,
                double currentBaseFps, double baselineOutputFps,
                double currentOutputFps) override {
            logAdaptiveBridgeResult(
                accepted, baselineLimit, testedLimit, baselineBaseFps,
                currentBaseFps, baselineOutputFps, currentOutputFps
            );
        }
        void probeAborted(std::string_view reason,
                size_t testedLimit) override {
            logAdaptiveProbeAborted(reason, testedLimit);
        }
        void rearm(std::string_view operation, std::string_view reason,
                size_t failures, size_t fallbackLimit,
                std::chrono::steady_clock::duration cooldown,
                double baselineBaseFps, double currentBaseFps,
                std::string_view decision) override {
            logAdaptiveRearm(
                operation, reason, failures, fallbackLimit, cooldown,
                baselineBaseFps, currentBaseFps, decision
            );
        }
        void fastCadenceBurst(double baselineBaseFps,
                double instantaneousBaseFps, double thresholdFps,
                size_t ignoredFrames, size_t totalIgnoredFrames,
                std::chrono::steady_clock::duration duration) override {
            logAdaptiveFastCadenceBurst(
                baselineBaseFps, instantaneousBaseFps, thresholdFps,
                ignoredFrames, totalIgnoredFrames, duration
            );
        }
        void fastCadenceBurstComplete(size_t totalIgnoredFrames,
                std::chrono::steady_clock::duration duration) override {
            logAdaptiveFastCadenceBurstComplete(totalIgnoredFrames, duration);
        }
        void rampBackoff(size_t testedLimit, size_t failures,
                double baselineBaseFps,
                std::chrono::steady_clock::duration delay) override {
            logAdaptiveRampBackoff(
                testedLimit, failures, baselineBaseFps, delay
            );
        }
        void rampEarlyRetry(size_t testedLimit, double failedBaseFps,
                double currentBaseFps) override {
            logAdaptiveRampEarlyRetry(
                testedLimit, failedBaseFps, currentBaseFps
            );
        }
        void recoveryResume(size_t generationLimit,
                std::chrono::steady_clock::duration higherProbeDelay,
                std::string_view reason) override {
            logAdaptiveRecoveryResume(
                generationLimit, higherProbeDelay, reason
            );
        }
        void stableCadence(std::string_view operation,
                size_t generatedLimit, double baselineBaseFps,
                double currentBaseFps, std::string_view reason) override {
            logAdaptiveStableCadence(
                operation, generatedLimit, baselineBaseFps,
                currentBaseFps, reason
            );
        }
        void rescueStart(size_t generatedLimit, double baselineBaseFps,
                double currentBaseFps, double projectedOutputFps,
                std::string_view reason) override {
            logAdaptiveRescueStart(
                generatedLimit, baselineBaseFps, currentBaseFps,
                projectedOutputFps, reason
            );
        }
        void rescueComplete(size_t previousLimit, size_t resumedLimit,
                size_t requestedLimit, size_t configuredLimit,
                double baselineBaseFps, double measuredBaseFps,
                std::string_view decision) override {
            logAdaptiveRescueComplete(
                previousLimit, resumedLimit, requestedLimit, configuredLimit,
                baselineBaseFps, measuredBaseFps, decision
            );
        }
        void discontinuityRecoveryStart(size_t generationLimit,
                double baselineBaseFps, std::string_view reason,
                std::chrono::steady_clock::duration maximumDuration) override {
            logAdaptiveDiscontinuityRecoveryStart(
                generationLimit, baselineBaseFps, reason, maximumDuration
            );
        }
        void discontinuityRecoveryComplete(size_t generationLimit,
                double baselineBaseFps, double measuredBaseFps,
                std::string_view decision) override {
            logAdaptiveDiscontinuityRecoveryComplete(
                generationLimit, baselineBaseFps, measuredBaseFps, decision
            );
        }
        void twoXGameplayHitchRecovery(size_t generationLimit,
                double baselineBaseFps,
                std::chrono::steady_clock::duration rawInterval) override {
            logAdaptiveTwoXGameplayHitchRecovery(
                generationLimit, baselineBaseFps, rawInterval
            );
        }
    };

    SwapchainAdaptiveSchedulerDiagnostics adaptiveSchedulerDiagnostics;

    void logSwapchainRecreationSuppressed(const std::string_view reason,
            const double remainingMs) {
        if (!presentDiagnosticsEnabled())
            return;

        std::cerr << "lsfg-vk: present diagnostics: operation=swapchain-recreation-suppressed"
                  << " context=" << activeDiagnosticsContextId
                  << " reason=" << reason
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
            const size_t recoveryLoadFallbackGenerationLimit,
            const double recoveryLoadBaselineBaseFps,
            const bool discontinuityRecoveryContext,
            const size_t discontinuityFallbackGenerationLimit,
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

    if (this->profile.adaptive) {
        this->adaptiveScheduler.emplace(
            AdaptiveSchedulerConfig{
                .targetFps = this->profile.target_fps,
                .maximumMultiplier = this->profile.adaptive_max_multiplier,
                .generatedFrameCapacity = this->destinationImages.size(),
                .stableCadence = this->profile.adaptive_stable_cadence,
            },
            &adaptiveSchedulerDiagnostics
        );
    }

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
        const auto schedulerNow = DiagnosticsClock::now();
        this->adaptiveScheduler->beginStabilization(
            schedulerNow,
            recoveryContext ? "swapchain-recreation" : "startup"
        );
        if (recoveryContext) {
            if (discontinuityRecoveryContext) {
                this->adaptiveScheduler->beginDiscontinuityRecovery(
                    schedulerNow,
                    recoveryGenerationLimit,
                    discontinuityFallbackGenerationLimit,
                    discontinuityBaselineBaseFps,
                    discontinuityDeadline,
                    discontinuitySoftRecoveryAttempted,
                    "swapchain-recreation"
                );
            } else {
                this->adaptiveScheduler->restoreGenerationLimit(
                    schedulerNow,
                    recoveryGenerationLimit,
                    "swapchain-recreation",
                    recoveryLoadFallbackGenerationLimit,
                    recoveryLoadBaselineBaseFps
                );
            }
        }
    }
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
    const bool historyWarmupActive = this->adaptiveScheduler &&
        this->adaptiveScheduler->historyWarmupActive();
    const auto generatedFramePlan = this->profile.adaptive && !historyWarmupActive
        ? this->adaptiveScheduler->planFrame(
            DiagnosticsClock::now(), this->generatedImageAcquireBackoff
        )
        : AdaptiveFramePlan{};
    const size_t generatedFrameCount = this->profile.adaptive
        ? generatedFramePlan.size()
        : this->destinationImages.size();

    const auto configuredAcquireTimeout = generatedImageAcquireTimeoutNs();
    bool renderFencePrepared = false;
    bool bypassGeneratedFrames = historyWarmupActive ||
        (this->profile.adaptive && generatedFramePlan.empty());
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
                this->adaptiveScheduler->discontinuityRecoveryActive();
            size_t recoveryGenerationLimit = 0;
            AdaptiveGenerationLoadBaseline recoveryLoadBaseline;
            if (this->profile.adaptive) {
                recoveryGenerationLimit = discontinuityRecoveryActive
                    ? this->adaptiveScheduler->discontinuityGenerationLimit()
                    : this->adaptiveScheduler->validatedGenerationLimit();
                recoveryLoadBaseline =
                    this->adaptiveScheduler->generationLoadBaseline();
            }
            const bool useSoftDiscontinuityRecovery =
                discontinuityRecoveryActive &&
                !this->adaptiveScheduler->discontinuitySoftRecoveryAttempted();
            if (useSoftDiscontinuityRecovery) {
                this->adaptiveScheduler->markDiscontinuitySoftRecoveryAttempted();
                logAdaptiveDiscontinuitySoftRecovery(recoveryGenerationLimit);
            }
            if (this->profile.adaptive && presentRecoveryRecreateEnabled()) {
                const auto recoveryNow = DiagnosticsClock::now();
                const auto recoveryDecision = this->adaptiveRecoveryState
                    ? this->adaptiveRecoveryState->presentationRecoveryPolicy.
                        recover(recoveryNow, true)
                    : AdaptivePresentationRecoveryDecision{};
                requestSwapchainRecreation = recoveryDecision.action ==
                    AdaptivePresentationRecoveryAction::RecreateSwapchain;
                if (requestSwapchainRecreation && this->adaptiveRecoveryState) {
                    this->adaptiveRecoveryState->nextContextIsRecovery = true;
                    this->adaptiveRecoveryState->nextContextGenerationLimit =
                        recoveryGenerationLimit;
                    this->adaptiveRecoveryState->
                        nextContextLoadFallbackGenerationLimit =
                            recoveryLoadBaseline.fallbackGenerationLimit;
                    this->adaptiveRecoveryState->nextContextLoadBaselineBaseFps =
                        recoveryLoadBaseline.baseFps;
                    this->adaptiveRecoveryState->nextContextIsDiscontinuityRecovery =
                        discontinuityRecoveryActive;
                    this->adaptiveRecoveryState->
                        nextContextDiscontinuityFallbackGenerationLimit =
                            this->adaptiveScheduler->
                                discontinuityFallbackGenerationLimit();
                    this->adaptiveRecoveryState->
                        nextContextDiscontinuityBaselineBaseFps =
                            this->adaptiveScheduler->
                                discontinuityBaselineBaseFps();
                    this->adaptiveRecoveryState->nextContextDiscontinuityDeadline =
                        this->adaptiveScheduler->discontinuityDeadline();
                    this->adaptiveRecoveryState->
                        nextContextDiscontinuitySoftRecoveryAttempted =
                            this->adaptiveScheduler->
                                discontinuitySoftRecoveryAttempted();
                } else if (recoveryDecision.action ==
                        AdaptivePresentationRecoveryAction::InPlaceCooldown) {
                    logSwapchainRecreationSuppressed(
                        "cooldown",
                        std::chrono::duration<double, std::milli>(
                            recoveryDecision.recreationCooldownRemaining
                        ).count()
                    );
                } else if (!useSoftDiscontinuityRecovery) {
                    logSwapchainRecreationSuppressed(
                        "first-recovery",
                        std::chrono::duration<double, std::milli>(
                            AdaptivePresentationRecoveryPolicy::
                                repeatedRecoveryWindow()
                        ).count()
                    );
                }
            }
            const size_t recoveryWarmupFrames = this->profile.adaptive &&
                    !requestSwapchainRecreation
                ? AdaptiveScheduler::historyWarmupFrameCount()
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
                this->adaptiveScheduler->beginStabilization(
                    recoveryNow, "generated-image-recovery"
                );
                if (!discontinuityRecoveryActive) {
                    this->adaptiveScheduler->restoreGenerationLimit(
                        recoveryNow,
                        recoveryGenerationLimit,
                        "generated-image-recovery",
                        recoveryLoadBaseline.fallbackGenerationLimit,
                        recoveryLoadBaseline.baseFps
                    );
                }
            } else {
                if (this->adaptiveScheduler)
                    this->adaptiveScheduler->resetTiming(DiagnosticsClock::now());
            }
            if (recoveryWarmupFrames || requestSwapchainRecreation) {
                // The successful probe owns a swapchain image. Copy the real
                // image into it and present it below before either warming the
                // current context or asking the game to recreate that context.
                recoveryWarmupImage = recoveryImageIndex;
                if (this->adaptiveScheduler) {
                    this->adaptiveScheduler->beginHistoryWarmup(
                        recoveryWarmupFrames, true
                    );
                }
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
                this->instance.get().scheduleFrames(
                    this->ctx.get(), generatedFramePlan.timestamps()
                );
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
        const std::array<VkSemaphore, 1> fallbackWaitSemaphores{
            fallbackPass.acquireSemaphore.handle()
        };
        const std::array<VkSemaphore, 2> fallbackSignalSemaphores{
            fallbackSemaphores.first.handle(), fallbackSemaphore.handle()
        };
        const std::span<const VkSemaphore> fallbackWaits = recoveryWarmupImage
            ? std::span<const VkSemaphore>{fallbackWaitSemaphores}
            : std::span<const VkSemaphore>{};
        const std::span<const VkSemaphore> fallbackSignals = recoveryWarmupImage
            ? std::span<const VkSemaphore>{fallbackSignalSemaphores}
            : std::span<const VkSemaphore>{fallbackSignalSemaphores.data(), 1};
        fallbackCommandBuffer.submit(vk,
            fallbackWaits, this->syncSemaphore->handle(), sourceTimelineValue,
            fallbackSignals, VK_NULL_HANDLE, 0,
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

        if (this->adaptiveScheduler &&
                this->adaptiveScheduler->historyWarmupActive()) {
            logHistoryWarmup(
                this->fidx, this->idx,
                this->adaptiveScheduler->historyWarmupRemaining(),
                this->adaptiveScheduler->historyWarmupIsRecovery(),
                recoveryWarmupImage
            );
            this->adaptiveScheduler->consumeHistoryWarmupFrame(
                DiagnosticsClock::now()
            );
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
            if (this->adaptiveScheduler) {
                this->adaptiveScheduler->cancelHistoryWarmup();
                this->adaptiveScheduler->resetTiming(
                    this->generatedImageAcquireLastBoundedProbe.value()
                );
            }

            auto& fallbackCommandBuffer = pass.commandBuffer;
            fallbackCommandBuffer.begin(vk);
            fallbackCommandBuffer.end(vk);
            const std::array<VkSemaphore, 1> fallbackSignals{fallbackSemaphore.handle()};
            fallbackCommandBuffer.submit(vk,
                {}, this->syncSemaphore->handle(), finalGeneratedTimelineValue,
                fallbackSignals, VK_NULL_HANDLE, 0,
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

        std::array<VkSemaphore, 2> waitSemaphores{pass.acquireSemaphore.handle()};
        size_t waitSemaphoreCount{1};
        if (i) { // non-first pass
            const auto& prevPCS = this->postCopySemaphores.at((this->idx - 1) % this->postCopySemaphores.size());
            waitSemaphores.at(waitSemaphoreCount++) = prevPCS.second.handle();
        }

        const std::array<VkSemaphore, 2> signalSemaphores{
            pcs.first.handle(),
            pcs.second.handle()
        };

        cmdbuf.end(vk);
        const auto generatedSubmitStarted = startPresentDiagnostic();
        cmdbuf.submit(vk,
            std::span<const VkSemaphore>{waitSemaphores.data(), waitSemaphoreCount},
            this->syncSemaphore->handle(), this->idx,
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
