/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "adaptive_scheduler.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace lsfgvk::layer;
using namespace std::chrono_literals;

namespace {
    using TimePoint = AdaptiveScheduler::TimePoint;

    struct TestFailure {
        std::string message;
    };

    void require(const bool condition, std::string message) {
        if (!condition)
            throw TestFailure{std::move(message)};
    }

    void requireNear(const float actual, const float expected,
            const float tolerance, std::string message) {
        if (std::abs(actual - expected) > tolerance) {
            message += ": expected " + std::to_string(expected) +
                ", got " + std::to_string(actual);
            throw TestFailure{std::move(message)};
        }
    }

    struct RecordingDiagnostics final : AdaptiveSchedulerDiagnostics {
        struct Event {
            std::string operation;
            std::string reason;
            bool accepted{false};
            size_t previousLimit{0};
            size_t testedLimit{0};
        };

        std::vector<Event> events;

        [[nodiscard]] bool enabled() const override { return true; }

        void stabilization(const std::string_view reason,
                std::chrono::steady_clock::duration) override {
            this->events.push_back({
                .operation = "stabilization",
                .reason = std::string(reason),
            });
        }

        void ramp(const size_t previousLimit, const size_t testedLimit,
                double) override {
            this->events.push_back({
                .operation = "ramp",
                .reason = {},
                .previousLimit = previousLimit,
                .testedLimit = testedLimit,
            });
        }

        void rampResult(const bool accepted, const size_t previousLimit,
                const size_t testedLimit, double, double, double,
                double) override {
            this->events.push_back({
                .operation = "ramp-result",
                .reason = {},
                .accepted = accepted,
                .previousLimit = previousLimit,
                .testedLimit = testedLimit,
            });
        }

        void probeAborted(std::string_view reason, size_t testedLimit) override {
            this->events.push_back({
                .operation = "probe-aborted",
                .reason = std::string(reason),
                .testedLimit = testedLimit,
            });
        }

        void bridge(size_t previousLimit, size_t testedLimit,
                size_t bridgeLimit, double, double, double, double) override {
            this->events.push_back({
                .operation = "bridge",
                .reason = {},
                .previousLimit = previousLimit,
                .testedLimit = bridgeLimit,
            });
            static_cast<void>(testedLimit);
        }

        void bridgeResult(bool accepted, size_t baselineLimit,
                size_t testedLimit, double, double, double, double) override {
            this->events.push_back({
                .operation = "bridge-result",
                .reason = {},
                .accepted = accepted,
                .previousLimit = baselineLimit,
                .testedLimit = testedLimit,
            });
        }

        void rearm(std::string_view operation, std::string_view reason,
                size_t, size_t, std::chrono::steady_clock::duration,
                double, double, std::string_view) override {
            this->events.push_back({
                .operation = std::string(operation),
                .reason = std::string(reason),
            });
        }

        void rampBackoff(size_t testedLimit, size_t failures, double,
                std::chrono::steady_clock::duration) override {
            this->events.push_back({
                .operation = "ramp-backoff",
                .reason = {},
                .previousLimit = failures,
                .testedLimit = testedLimit,
            });
        }

        void rampEarlyRetry(size_t testedLimit, double, double) override {
            this->events.push_back({
                .operation = "ramp-early-retry",
                .reason = {},
                .testedLimit = testedLimit,
            });
        }

        void fastCadenceBurst(double, double, double, size_t, size_t,
                std::chrono::steady_clock::duration) override {
            this->events.push_back({
                .operation = "fast-burst",
                .reason = {},
            });
        }

        void fastCadenceBurstComplete(size_t,
                std::chrono::steady_clock::duration) override {
            this->events.push_back({
                .operation = "fast-burst-complete",
                .reason = {},
            });
        }

        void twoXGameplayHitchRecovery(size_t, double,
                std::chrono::steady_clock::duration) override {
            this->events.push_back({
                .operation = "2x-hitch-recovery",
                .reason = {},
            });
        }

        void stableCadence(const std::string_view operation, size_t, double,
                double, const std::string_view reason) override {
            this->events.push_back({
                .operation = std::string(operation),
                .reason = std::string(reason),
            });
        }

        void rescueStart(size_t previousLimit, double, double, double,
                std::string_view reason) override {
            this->events.push_back({
                .operation = "rescue-start",
                .reason = std::string(reason),
                .previousLimit = previousLimit,
            });
        }

        void rescueComplete(size_t previousLimit, size_t resumedLimit,
                size_t, size_t, double, double,
                std::string_view decision) override {
            this->events.push_back({
                .operation = "rescue-complete",
                .reason = std::string(decision),
                .previousLimit = previousLimit,
                .testedLimit = resumedLimit,
            });
        }

        void discontinuityRecoveryComplete(size_t previousLimit, double,
                double, std::string_view decision) override {
            this->events.push_back({
                .operation = "discontinuity-complete",
                .reason = std::string(decision),
                .previousLimit = previousLimit,
            });
        }

        [[nodiscard]] bool contains(const std::string_view operation) const {
            for (const auto& event : this->events) {
                if (event.operation == operation)
                    return true;
            }
            return false;
        }

        [[nodiscard]] const Event* last(const std::string_view operation) const {
            for (auto event = this->events.rbegin();
                    event != this->events.rend(); ++event) {
                if (event->operation == operation)
                    return &*event;
            }
            return nullptr;
        }
    };

    struct Harness {
        RecordingDiagnostics diagnostics;
        AdaptiveScheduler scheduler;
        TimePoint now{};

        Harness(const uint32_t targetFps, const size_t maximumMultiplier,
                const bool stableCadence = false) :
            scheduler(
                AdaptiveSchedulerConfig{
                    .targetFps = targetFps,
                    .maximumMultiplier = maximumMultiplier,
                    .generatedFrameCapacity = 3,
                    .stableCadence = stableCadence,
                },
                &this->diagnostics
            ) {}

        void start() {
            this->scheduler.beginStabilization(this->now, "startup");
            for (size_t i = 0;
                    i < AdaptiveScheduler::historyWarmupFrameCount(); ++i) {
                this->now += 16ms;
                this->scheduler.consumeHistoryWarmupFrame(this->now);
            }
        }

        AdaptiveFramePlan frame(const std::chrono::nanoseconds interval,
                const bool acquireBackoff = false) {
            this->now += interval;
            return this->scheduler.planFrame(this->now, acquireBackoff);
        }

        AdaptiveFramePlan frameAtFps(const double fps,
                const bool acquireBackoff = false) {
            return this->frame(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::duration<double>(1.0 / fps)
            ), acquireBackoff);
        }

        AdaptiveFramePlan runAtFps(const double fps,
                const std::chrono::seconds duration) {
            AdaptiveFramePlan result;
            const size_t frames = static_cast<size_t>(
                std::ceil(fps * static_cast<double>(duration.count()))
            );
            for (size_t i = 0; i < frames; ++i)
                result = this->frameAtFps(fps);
            return result;
        }
    };

    void requireValidTimestamps(const AdaptiveFramePlan& timestamps,
            const size_t capacity) {
        require(timestamps.size() <= capacity,
            "generated timestamps exceeded destination capacity");
        float previous = 0.0F;
        for (const float timestamp : timestamps) {
            require(timestamp > previous,
                "generated timestamps were not strictly increasing");
            require(timestamp < 1.0F,
                "generated timestamp reached or exceeded the real frame");
            previous = timestamp;
        }
    }

    void testStartupWarmupIsExplicit() {
        Harness harness(120, 3);
        require(harness.scheduler.historyWarmupRemaining() == 3,
            "Adaptive must start with three temporal-history frames");
        require(harness.scheduler.snapshot().phase ==
                AdaptiveSchedulerPhase::HistoryWarmup,
            "startup history warm-up was not exposed as scheduler state");
        harness.start();
        require(!harness.scheduler.historyWarmupActive(),
            "startup history warm-up did not complete deterministically");
        require(harness.scheduler.snapshot().phase ==
                AdaptiveSchedulerPhase::Stabilizing,
            "scheduler left startup stabilization too early");
    }

    void testInvalidConfigurationIsRejectedAtBoundary() {
        bool invalidTargetRejected = false;
        try {
            AdaptiveScheduler scheduler({
                .targetFps = 0,
                .maximumMultiplier = 3,
                .generatedFrameCapacity = 3,
            });
            static_cast<void>(scheduler);
        } catch (const std::invalid_argument&) {
            invalidTargetRejected = true;
        }
        require(invalidTargetRejected,
            "scheduler accepted an invalid target FPS");

        bool invalidMultiplierRejected = false;
        try {
            AdaptiveScheduler scheduler({
                .targetFps = 120,
                .maximumMultiplier = 5,
                .generatedFrameCapacity = 3,
            });
            static_cast<void>(scheduler);
        } catch (const std::invalid_argument&) {
            invalidMultiplierRejected = true;
        }
        require(invalidMultiplierRejected,
            "scheduler accepted an invalid maximum multiplier");

        bool invalidCapacityRejected = false;
        try {
            AdaptiveScheduler scheduler({
                .targetFps = 120,
                .maximumMultiplier = 4,
                .generatedFrameCapacity = 4,
            });
            static_cast<void>(scheduler);
        } catch (const std::invalid_argument&) {
            invalidCapacityRejected = true;
        }
        require(invalidCapacityRejected,
            "scheduler accepted more than three generated-frame slots");
    }

    void testSteadySixtyRampsToTwoXFor120Target() {
        Harness harness(120, 3);
        harness.start();
        const auto timestamps = harness.runAtFps(60.0, 7s);
        requireValidTimestamps(timestamps, 3);
        require(timestamps.size() == 1,
            "60 FPS toward 120 FPS should settle at one generated frame");
        requireNear(timestamps.front(), 0.5F, 0.0001F,
            "2x interpolation timestamp changed");
        const auto snapshot = harness.scheduler.snapshot();
        require(snapshot.validatedGenerationLimit == 1,
            "2x level was not validated after its evaluation window");
        require(!snapshot.rampEvaluationActive,
            "steady 2x policy remained in a transient probe");
    }

    void testFourXPlanUsesEvenInterpolationTimestamps() {
        Harness harness(120, 4);
        harness.start();
        const auto timestamps = harness.runAtFps(30.0, 10s);
        requireValidTimestamps(timestamps, 3);
        require(timestamps.size() == 3,
            "30 FPS toward 120 FPS did not settle at the 4x ceiling");
        requireNear(timestamps[0], 0.25F, 0.0001F,
            "first 4x interpolation timestamp changed");
        requireNear(timestamps[1], 0.50F, 0.0001F,
            "second 4x interpolation timestamp changed");
        requireNear(timestamps[2], 0.75F, 0.0001F,
            "third 4x interpolation timestamp changed");
    }

    void testSchedulerCannotReduceAboveTargetCadence() {
        Harness harness(120, 4);
        harness.start();
        const auto timestamps = harness.runAtFps(144.0, 6s);
        require(timestamps.empty(),
            "scheduler generated frames when real cadence exceeded target");
        require(harness.scheduler.snapshot().validatedGenerationLimit == 0,
            "above-target cadence raised the validated generation level");
    }

    void testAcquireBackoffDoesNotAdvancePolicy() {
        Harness harness(120, 3);
        harness.start();
        harness.runAtFps(60.0, 7s);
        const auto before = harness.scheduler.snapshot();
        for (size_t i = 0; i < 120; ++i) {
            const auto timestamps = harness.frameAtFps(60.0, true);
            require(timestamps.size() == 1,
                "acquire backoff must perform exactly one availability probe");
            requireNear(timestamps.front(), 0.5F, 0.0001F,
                "acquire-backoff probe timestamp changed");
        }
        const auto after = harness.scheduler.snapshot();
        require(after.generationLimit == before.generationLimit,
            "acquire backoff advanced generation policy");
        require(after.validatedGenerationLimit ==
                before.validatedGenerationLimit,
            "acquire backoff changed the validated level");
    }

    void testValidatedTwoXSurvivesShortGameplayHitch() {
        Harness harness(90, 2);
        harness.start();
        harness.runAtFps(45.0, 7s);
        require(harness.scheduler.snapshot().validatedGenerationLimit == 1,
            "precondition failed: 2x was not validated");

        const auto hitchPlan = harness.frame(200ms);
        require(hitchPlan.empty(),
            "short hitch recovery generated from stale temporal history");
        require(harness.scheduler.historyWarmupRemaining() == 3,
            "short 2x hitch did not request a full history refresh");
        require(harness.scheduler.snapshot().validatedGenerationLimit == 1,
            "short 2x hitch discarded its validated policy");
        require(harness.diagnostics.contains("2x-hitch-recovery"),
            "short 2x hitch recovery was not observable");
    }

    void testLongHitchUsesConservativeDiscontinuityRecovery() {
        Harness harness(90, 2);
        harness.start();
        harness.runAtFps(45.0, 7s);
        const auto hitchPlan = harness.frame(400ms);
        require(hitchPlan.empty(),
            "long hitch unexpectedly generated a frame");
        const auto snapshot = harness.scheduler.snapshot();
        require(snapshot.discontinuityRecoveryActive,
            "long hitch bypassed menu/focus discontinuity recovery");
        require(snapshot.generationLimit == 0,
            "long hitch retained generated load during stabilization");
    }

    void testRecoveredCadenceRestoresValidatedLevel() {
        Harness harness(90, 2);
        harness.start();
        harness.runAtFps(45.0, 7s);
        require(harness.scheduler.snapshot().validatedGenerationLimit == 1,
            "precondition failed: 2x was not validated");
        harness.frame(400ms);
        require(harness.scheduler.discontinuityRecoveryActive(),
            "precondition failed: long hitch did not start recovery");

        harness.runAtFps(45.0, 3s);
        const auto snapshot = harness.scheduler.snapshot();
        require(!snapshot.discontinuityRecoveryActive,
            "healthy cadence did not complete discontinuity recovery");
        require(snapshot.validatedGenerationLimit == 1,
            "healthy cadence did not restore the proven 2x level");
        require(harness.frameAtFps(45.0).size() == 1,
            "restored 2x policy did not resume generation");
    }

    void testDiscontinuityRecoveryTimesOutToFreshRamp() {
        Harness harness(90, 2);
        harness.start();
        harness.runAtFps(45.0, 7s);
        harness.frame(400ms);
        require(harness.scheduler.discontinuityRecoveryActive(),
            "precondition failed: long hitch did not start recovery");

        for (size_t frame = 0;
                frame < 160 &&
                    !harness.diagnostics.contains("discontinuity-complete");
                ++frame) {
            harness.frameAtFps(20.0);
        }
        const auto* completion = harness.diagnostics.last(
            "discontinuity-complete"
        );
        require(completion && completion->reason == "timeout-ramp-from-zero",
            "unrecovered cadence did not take the bounded timeout path");
        const auto snapshot = harness.scheduler.snapshot();
        require(!snapshot.discontinuityRecoveryActive,
            "expired discontinuity recovery remained active");
        require(snapshot.generationLimit == 0,
            "expired discontinuity recovery retained stale generated load");
    }

    void testSustainedCadenceDropRebasesWithoutMenuRecovery() {
        Harness harness(120, 3);
        harness.start();
        harness.runAtFps(60.0, 7s);
        require(harness.scheduler.snapshot().validatedGenerationLimit == 1,
            "precondition failed: 2x was not validated");

        harness.frameAtFps(30.0);
        harness.frameAtFps(30.0);
        harness.frameAtFps(30.0);
        const auto snapshot = harness.scheduler.snapshot();
        require(snapshot.phase == AdaptiveSchedulerPhase::Stabilizing,
            "sustained gameplay cadence drop did not rebase through stabilization");
        require(!snapshot.discontinuityRecoveryActive,
            "ordinary gameplay cadence drop was mistaken for a menu discontinuity");
        require(snapshot.generationLimit == 0,
            "cadence rebase retained load before measuring the new scene");
    }

    void testImpossibleFastBurstDoesNotCorruptCadence() {
        Harness harness(120, 3);
        harness.start();
        harness.runAtFps(60.0, 7s);
        const auto before = harness.scheduler.snapshot();
        for (size_t i = 0; i < 30; ++i)
            require(harness.frame(1ms).empty(),
                "impossible fast-present burst generated interpolation work");
        harness.frameAtFps(60.0);
        const auto after = harness.scheduler.snapshot();
        require(after.validatedGenerationLimit ==
                before.validatedGenerationLimit,
            "fast-present burst corrupted the proven generation level");
        require(after.smoothedBaseFps < 80.0,
            "fast-present burst polluted the gameplay cadence estimate");
        require(harness.diagnostics.contains("fast-burst-complete"),
            "fast-present burst completion was not observable");
    }

    void testRejectedFirstProbeEntersBoundedRearm() {
        Harness harness(180, 2);
        harness.start();
        for (size_t frame = 0;
                frame < 600 &&
                    !harness.scheduler.snapshot().rampEvaluationActive;
                ++frame) {
            harness.frameAtFps(60.0);
        }
        require(harness.scheduler.snapshot().rampEvaluationActive,
            "precondition failed: initial multiplier probe did not begin");
        // 34 FPS is slow enough to make 2x counterproductive against the
        // 60 FPS baseline, but not slow enough to trip the separate 2x raw
        // cadence-discontinuity detector before the one-second probe ends.
        harness.runAtFps(34.0, 2s);
        const auto snapshot = harness.scheduler.snapshot();
        require(snapshot.rearmRequired,
            "harmful first multiplier probe did not enter rearm cooldown");
        require(snapshot.validatedGenerationLimit == 0,
            "rejected first probe was incorrectly treated as validated");
        const auto* result = harness.diagnostics.last("ramp-result");
        require(result && !result->accepted,
            "rejected probe result was not emitted deterministically");
    }

    void testInterruptedProbeRearmsWithoutFailurePenalty() {
        Harness harness(180, 3);
        harness.start();
        for (size_t frame = 0;
                frame < 600 &&
                    !harness.scheduler.snapshot().rampEvaluationActive;
                ++frame) {
            harness.frameAtFps(60.0);
        }
        require(harness.scheduler.snapshot().rampEvaluationActive,
            "precondition failed: multiplier probe did not begin");

        harness.frame(400ms);
        auto snapshot = harness.scheduler.snapshot();
        require(snapshot.rearmRequired,
            "interrupted probe did not enter bounded rearm");
        const auto* scheduled = harness.diagnostics.last(
            "adaptive-rearm-scheduled"
        );
        require(scheduled && scheduled->reason == "probe-interrupted",
            "interrupted probe was recorded as a throughput rejection");

        harness.runAtFps(60.0, 4s);
        snapshot = harness.scheduler.snapshot();
        require(!snapshot.rearmRequired,
            "stable cadence did not rearm an interrupted probe promptly");
        const auto* ready = harness.diagnostics.last("adaptive-rearm-ready");
        require(ready && ready->reason == "probe-interrupted",
            "interrupted probe rearm decision was not observable");
    }

    void testBridgeProbeCanRecoverMisleadingFirstStep() {
        Harness harness(180, 3);
        harness.start();
        for (size_t frame = 0;
                frame < 600 &&
                    !harness.scheduler.snapshot().rampEvaluationActive;
                ++frame) {
            harness.frameAtFps(60.0);
        }
        require(harness.scheduler.snapshot().rampEvaluationActive,
            "precondition failed: initial multiplier probe did not begin");

        harness.runAtFps(31.0, 3s);
        require(harness.diagnostics.contains("bridge"),
            "counterproductive-looking first step did not start a bridge probe");
        const auto* result = harness.diagnostics.last("bridge-result");
        require(result && result->accepted,
            "useful bridge multiplier was not accepted");
        require(harness.scheduler.snapshot().validatedGenerationLimit == 2,
            "accepted bridge multiplier was not retained as validated");
    }

    void testRejectedHigherLevelRetainsProvenLoadAndBacksOff() {
        Harness harness(180, 3);
        harness.start();
        for (size_t frame = 0;
                frame < 900;
                ++frame) {
            harness.frameAtFps(60.0);
            const auto snapshot = harness.scheduler.snapshot();
            if (snapshot.rampEvaluationActive &&
                    snapshot.generationLimit == 2 &&
                    snapshot.validatedGenerationLimit == 1) {
                break;
            }
        }
        auto snapshot = harness.scheduler.snapshot();
        require(snapshot.rampEvaluationActive &&
                snapshot.generationLimit == 2 &&
                snapshot.validatedGenerationLimit == 1,
            "precondition failed: higher-level probe did not begin from proven 2x");

        harness.runAtFps(35.0, 2s);
        snapshot = harness.scheduler.snapshot();
        require(!snapshot.rampEvaluationActive,
            "counterproductive higher-level probe did not finish");
        require(snapshot.validatedGenerationLimit == 1,
            "rejected higher-level probe discarded the proven 2x load");
        const auto* backoff = harness.diagnostics.last("ramp-backoff");
        require(backoff && backoff->testedLimit == 2 &&
                backoff->previousLimit == 1,
            "higher-level rejection did not schedule first bounded retry delay");
    }

    void testSmoothCadenceSettlesNearIntegerDemand() {
        Harness harness(90, 2, true);
        harness.start();
        const auto timestamps = harness.runAtFps(47.0, 10s);
        require(timestamps.size() == 1,
            "Smooth Cadence did not settle on constant 2x output");
        require(harness.scheduler.snapshot().phase ==
                AdaptiveSchedulerPhase::StableCadence,
            "accepted Smooth Cadence was not exposed as scheduler state");
        require(harness.diagnostics.contains(
                "adaptive-stable-cadence-accepted"),
            "Smooth Cadence acceptance was not observable");
    }

    void testSmoothCadenceReturnsToTargetAfterBaseRecovery() {
        Harness harness(100, 2, true);
        harness.start();
        harness.runAtFps(50.0, 10s);
        require(harness.scheduler.snapshot().phase ==
                AdaptiveSchedulerPhase::StableCadence,
            "precondition failed: 50 FPS did not settle on 2x for a 100 FPS target");

        harness.runAtFps(60.0, 3s);
        require(harness.scheduler.snapshot().phase !=
                AdaptiveSchedulerPhase::StableCadence,
            "Smooth Cadence retained stale 2x output after native cadence recovered");
        const auto* disabled = harness.diagnostics.last(
            "adaptive-stable-cadence-disabled"
        );
        require(disabled && disabled->reason == "outside-useful-range",
            "native cadence recovery did not report a bounded Smooth Cadence exit");

        size_t outputs = 0;
        constexpr size_t sampleFrames = 600;
        for (size_t frame = 0; frame < sampleFrames; ++frame)
            outputs += 1 + harness.frameAtFps(60.0).size();
        const double estimatedOutputFps =
            static_cast<double>(outputs) / 10.0;
        require(std::abs(estimatedOutputFps - 100.0) <= 0.2,
            "strict scheduling did not return recovered cadence to the 100 FPS target");
    }

    void testStrictLoadCollapseRestoresCheaperProvenLevel() {
        Harness harness(180, 3);
        harness.start();
        harness.runAtFps(60.0, 10s);
        require(harness.scheduler.snapshot().validatedGenerationLimit == 2,
            "precondition failed: 3x was not validated");

        for (size_t frame = 0;
                frame < 160 && !harness.diagnostics.contains("rescue-start");
                ++frame) {
            harness.frameAtFps(40.0);
        }
        const auto* rescueStart = harness.diagnostics.last("rescue-start");
        require(rescueStart && rescueStart->reason == "strict-load-collapse",
            "sustained high-multiplier throughput collapse did not start rescue");
        require(harness.scheduler.snapshot().phase ==
                AdaptiveSchedulerPhase::RescueMeasurement,
            "strict-load rescue was not exposed as scheduler state");

        harness.runAtFps(60.0, 2s);
        const auto* rescueComplete = harness.diagnostics.last("rescue-complete");
        require(rescueComplete &&
                rescueComplete->reason == "strict-load-restored",
            "real-only throughput recovery did not select the cheaper proven level");
        require(harness.scheduler.snapshot().validatedGenerationLimit == 1,
            "strict-load rescue did not restore the validated lower load");
    }

    void testSmoothCadenceCollapseUsesRealOnlyMeasurement() {
        Harness harness(90, 2, true);
        harness.start();
        harness.runAtFps(47.0, 10s);
        require(harness.scheduler.snapshot().phase ==
                AdaptiveSchedulerPhase::StableCadence,
            "precondition failed: Smooth Cadence did not settle");

        for (size_t frame = 0;
                frame < 120 && !harness.diagnostics.contains("rescue-start");
                ++frame) {
            harness.frameAtFps(30.0);
        }
        const auto* rescueStart = harness.diagnostics.last("rescue-start");
        require(rescueStart &&
                rescueStart->reason == "stable-cadence-collapse",
            "collapsed Smooth Cadence did not start real-only measurement");
        require(harness.scheduler.snapshot().phase ==
                AdaptiveSchedulerPhase::RescueMeasurement,
            "Smooth Cadence rescue was not exposed as scheduler state");
        require(harness.frameAtFps(30.0).empty(),
            "Smooth Cadence rescue generated during real-only measurement");
    }

    void testDeterministicReplay() {
        Harness first(120, 4, true);
        Harness second(120, 4, true);
        first.start();
        second.start();

        const std::vector<std::chrono::nanoseconds> trace{
            17ms, 16ms, 17ms, 16ms, 50ms, 16ms, 17ms, 200ms,
            16ms, 16ms, 17ms, 1ms, 1ms, 16ms, 33ms, 34ms,
        };
        for (size_t replay = 0; replay < 40; ++replay) {
            for (const auto interval : trace) {
                const auto firstPlan = first.frame(interval);
                const auto secondPlan = second.frame(interval);
                require(firstPlan == secondPlan,
                    "identical cadence trace produced different plans");
                requireValidTimestamps(firstPlan, 3);
            }
        }

        const auto firstSnapshot = first.scheduler.snapshot();
        const auto secondSnapshot = second.scheduler.snapshot();
        require(firstSnapshot.phase == secondSnapshot.phase &&
                firstSnapshot.generationLimit ==
                    secondSnapshot.generationLimit &&
                firstSnapshot.validatedGenerationLimit ==
                    secondSnapshot.validatedGenerationLimit &&
                firstSnapshot.historyWarmupRemaining ==
                    secondSnapshot.historyWarmupRemaining,
            "identical cadence trace produced different final state");
    }

    struct TestCase {
        std::string_view name;
        void (*run)();
    };
}

int main() {
    const std::vector<TestCase> tests{
        {"startup warm-up is explicit", testStartupWarmupIsExplicit},
        {"invalid configuration is rejected", testInvalidConfigurationIsRejectedAtBoundary},
        {"60 to 120 settles at 2x", testSteadySixtyRampsToTwoXFor120Target},
        {"4x timestamps remain evenly spaced", testFourXPlanUsesEvenInterpolationTimestamps},
        {"above-target cadence remains real-only", testSchedulerCannotReduceAboveTargetCadence},
        {"acquire backoff freezes policy", testAcquireBackoffDoesNotAdvancePolicy},
        {"validated 2x survives short hitch", testValidatedTwoXSurvivesShortGameplayHitch},
        {"long hitch uses discontinuity recovery", testLongHitchUsesConservativeDiscontinuityRecovery},
        {"healthy cadence restores validated level", testRecoveredCadenceRestoresValidatedLevel},
        {"discontinuity timeout restarts from zero", testDiscontinuityRecoveryTimesOutToFreshRamp},
        {"gameplay cadence drop rebases", testSustainedCadenceDropRebasesWithoutMenuRecovery},
        {"fast-present burst preserves cadence", testImpossibleFastBurstDoesNotCorruptCadence},
        {"harmful first probe enters rearm", testRejectedFirstProbeEntersBoundedRearm},
        {"interrupted probe rearms promptly", testInterruptedProbeRearmsWithoutFailurePenalty},
        {"bridge probe handles misleading first step", testBridgeProbeCanRecoverMisleadingFirstStep},
        {"rejected higher level backs off", testRejectedHigherLevelRetainsProvenLoadAndBacksOff},
        {"Smooth Cadence settles near integer demand", testSmoothCadenceSettlesNearIntegerDemand},
        {"Smooth Cadence exits after native recovery", testSmoothCadenceReturnsToTargetAfterBaseRecovery},
        {"strict load collapse restores lower level", testStrictLoadCollapseRestoresCheaperProvenLevel},
        {"Smooth Cadence collapse measures real-only", testSmoothCadenceCollapseUsesRealOnlyMeasurement},
        {"cadence replay is deterministic", testDeterministicReplay},
    };

    size_t failures = 0;
    for (const auto& test : tests) {
        try {
            test.run();
            std::cout << "PASS: " << test.name << '\n';
        } catch (const TestFailure& failure) {
            failures++;
            std::cerr << "FAIL: " << test.name << ": "
                      << failure.message << '\n';
        } catch (const std::exception& error) {
            failures++;
            std::cerr << "FAIL: " << test.name
                      << ": unexpected exception: " << error.what() << '\n';
        }
    }

    if (failures) {
        std::cerr << failures << " adaptive scheduler test(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << tests.size() << " adaptive scheduler tests passed\n";
    return EXIT_SUCCESS;
}
