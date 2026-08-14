/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <chrono>
#include <optional>

namespace lsfgvk::layer {

    class StableBooleanFeedback {
    public:
        using Clock = std::chrono::steady_clock;
        using TimePoint = Clock::time_point;

        explicit StableBooleanFeedback(
                const std::chrono::milliseconds settleDelay =
                    std::chrono::milliseconds(750)) :
            settleDelay(settleDelay) {}

        void seed(const std::optional<bool> value) {
            this->confirmed = value;
            this->candidate.reset();
            this->candidateSince.reset();
        }

        [[nodiscard]] std::optional<bool> observe(
                const std::optional<bool> value, const TimePoint now) {
            if (!value)
                return std::nullopt;

            if (this->confirmed && *this->confirmed == *value) {
                this->candidate.reset();
                this->candidateSince.reset();
                return std::nullopt;
            }

            if (!this->candidate || *this->candidate != *value) {
                this->candidate = value;
                this->candidateSince = now;
                return std::nullopt;
            }

            if (!this->candidateSince ||
                    now - *this->candidateSince < this->settleDelay)
                return std::nullopt;

            this->confirmed = this->candidate;
            this->candidate.reset();
            this->candidateSince.reset();
            return this->confirmed;
        }

        [[nodiscard]] std::optional<bool> value() const {
            return this->confirmed;
        }

    private:
        std::chrono::milliseconds settleDelay;
        std::optional<bool> confirmed;
        std::optional<bool> candidate;
        std::optional<TimePoint> candidateSince;
    };

}
