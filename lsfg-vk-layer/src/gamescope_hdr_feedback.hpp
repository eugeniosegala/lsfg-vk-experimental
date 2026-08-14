/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <memory>
#include <optional>
#include <string>

namespace lsfgvk::layer {

    struct GamescopeHdrFeedbackSample {
        std::optional<bool> active;
        std::string status;
        std::string display;
    };

    /// Monitors Gamescope's application-HDR feedback without adding a
    /// mandatory X11 link-time dependency or an X11 round trip to the frame
    /// presentation path.
    class GamescopeHdrFeedbackReader {
    public:
        GamescopeHdrFeedbackReader();
        ~GamescopeHdrFeedbackReader();

        GamescopeHdrFeedbackReader(const GamescopeHdrFeedbackReader&) = delete;
        GamescopeHdrFeedbackReader& operator=(const GamescopeHdrFeedbackReader&) = delete;
        GamescopeHdrFeedbackReader(GamescopeHdrFeedbackReader&&) noexcept;
        GamescopeHdrFeedbackReader& operator=(GamescopeHdrFeedbackReader&&) noexcept;

        /// Return the latest sample collected by the background monitor.
        [[nodiscard]] std::optional<bool> sample() const;

        /// Return the value plus a stable diagnostic reason. This makes an
        /// unavailable feedback path distinguishable from confirmed SDR.
        [[nodiscard]] GamescopeHdrFeedbackSample diagnosticSample() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };

}
