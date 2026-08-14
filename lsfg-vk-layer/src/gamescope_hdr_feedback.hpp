/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <algorithm>
#include <memory>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lsfgvk::layer {

    struct GamescopeHdrFeedbackSample {
        std::optional<bool> active;
        std::optional<uint32_t> refreshHz;
        bool gamescopeDetected{false};
        std::optional<uint32_t> gamescopePid;
        std::optional<uint32_t> xwaylandServerId;
        std::string status;
        std::string display;
        std::string resolverStatus;
        std::string resolverCandidates;
    };

    struct GamescopeXwaylandDisplay {
        std::string display;
        std::optional<uint32_t> gamescopePid;
        std::optional<uint32_t> serverId;
    };

    /// Choose Gamescope's server-zero Xwayland display from displays belonging
    /// to the same compositor process as the game's current display.
    [[nodiscard]] inline std::optional<std::string> selectGamescopeRootDisplay(
            const GamescopeXwaylandDisplay& current,
            const std::vector<GamescopeXwaylandDisplay>& candidates) {
        if (!current.gamescopePid || !current.serverId)
            return std::nullopt;
        if (*current.serverId == 0)
            return current.display;

        const auto candidate = std::ranges::find_if(candidates,
            [&current](const GamescopeXwaylandDisplay& value) {
                return value.gamescopePid == current.gamescopePid &&
                    value.serverId && *value.serverId == 0;
            });
        if (candidate == candidates.end())
            return std::nullopt;
        return candidate->display;
    }

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
