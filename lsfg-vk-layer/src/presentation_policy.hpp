/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace lsfgvk::layer {

    /// Bound a generated-image acquire to the time in which the synthetic
    /// frame can still occupy its intended display slot.
    [[nodiscard]] inline uint64_t generatedImageDeadlineNs(
            const std::optional<uint32_t> refreshHz,
            const std::optional<std::chrono::steady_clock::duration> realInterval,
            const size_t outputFrames,
            const std::optional<uint64_t> configuredMaximum) {
        constexpr uint64_t minimumNs = 1'000'000;
        constexpr uint64_t maximumNs = 12'000'000;
        constexpr uint64_t nanosecondsPerSecond = 1'000'000'000;

        uint64_t slotNs = maximumNs;
        if (refreshHz && *refreshHz > 0) {
            slotNs = nanosecondsPerSecond / *refreshHz;
        } else if (realInterval && outputFrames > 0) {
            const auto intervalNs = std::chrono::duration_cast<
                std::chrono::nanoseconds>(*realInterval).count();
            if (intervalNs > 0) {
                slotNs = static_cast<uint64_t>(intervalNs) /
                    static_cast<uint64_t>(outputFrames);
            }
        }

        // Leave a quarter of the display slot for the copy and present itself.
        uint64_t deadlineNs = std::clamp(
            slotNs * 3 / 4, minimumNs, maximumNs
        );
        if (configuredMaximum)
            deadlineNs = std::min(deadlineNs, *configuredMaximum);
        return std::max(deadlineNs, minimumNs);
    }

    /// Deterministically suppress synthetic frames which cannot be scanned out
    /// at the confirmed Gamescope refresh rate. Fixed mode remains at its full
    /// multiplier whenever that output fits the display budget.
    class FixedRefreshBudget {
    public:
        using TimePoint = std::chrono::steady_clock::time_point;

        [[nodiscard]] size_t plan(const TimePoint now,
                const std::optional<uint32_t> refreshHz,
                const size_t maximumGeneratedFrames) {
            if (!refreshHz || *refreshHz == 0 || maximumGeneratedFrames == 0) {
                this->lastRealFrame = now;
                return maximumGeneratedFrames;
            }
            if (!this->lastRealFrame) {
                this->lastRealFrame = now;
                return 0;
            }

            const double rawInterval = std::chrono::duration<double>(
                now - *this->lastRealFrame
            ).count();
            this->lastRealFrame = now;
            if (rawInterval <= 0.0 || rawInterval > 0.25) {
                this->smoothedIntervalSeconds = 0.0;
                this->outputCredit = 0.0;
                return 0;
            }
            if (this->smoothedIntervalSeconds == 0.0)
                this->smoothedIntervalSeconds = rawInterval;
            else
                this->smoothedIntervalSeconds =
                    this->smoothedIntervalSeconds * 0.75 + rawInterval * 0.25;

            const double desiredOutputs = std::clamp(
                this->smoothedIntervalSeconds * static_cast<double>(*refreshHz),
                1.0,
                static_cast<double>(maximumGeneratedFrames + 1)
            );
            this->outputCredit += desiredOutputs;
            const size_t requestedOutputs = std::max<size_t>(
                1, static_cast<size_t>(std::floor(this->outputCredit + 1e-9))
            );
            const size_t generated = std::min(
                requestedOutputs - 1, maximumGeneratedFrames
            );
            this->outputCredit -= static_cast<double>(generated + 1);
            if (this->outputCredit < 0.0)
                this->outputCredit = 0.0;
            if (generated == maximumGeneratedFrames && this->outputCredit >= 1.0)
                this->outputCredit = std::fmod(this->outputCredit, 1.0);
            return generated;
        }

        void reset() {
            this->lastRealFrame.reset();
            this->smoothedIntervalSeconds = 0.0;
            this->outputCredit = 0.0;
        }

    private:
        std::optional<TimePoint> lastRealFrame;
        double smoothedIntervalSeconds{0.0};
        double outputCredit{0.0};
    };

}
