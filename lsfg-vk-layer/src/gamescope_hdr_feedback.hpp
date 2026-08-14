/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <algorithm>
#include <memory>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lsfgvk::layer {

    struct GamescopeHdrFeedbackSample {
        std::optional<bool> active;
        std::optional<uint32_t> refreshHz;
        std::optional<bool> outputHdrEnabled;
        bool appHdrMetadataPresent{false};
        bool experimentalHdrRequested{false};
        bool gamescopeDetected{false};
        std::optional<uint32_t> gamescopePid;
        std::optional<uint32_t> xwaylandServerId;
        std::string status;
        std::string activationSource;
        std::string display;
        std::string resolverStatus;
        std::string resolverCandidates;
    };

    struct GamescopeHdrActivationEvidence {
        std::optional<bool> appWantsHdr;
        std::optional<bool> outputHdrEnabled;
        bool appHdrMetadataPresent{false};
        bool experimentalHdrRequested{false};
        bool hdrExposureDisabled{false};
        bool gamescopeDetected{false};
    };

    struct GamescopeHdrActivationDecision {
        std::optional<bool> active;
        std::string_view source{"unavailable"};
    };

    /// Resolve application HDR from independent, positive evidence. Gamescope
    /// deliberately leaves its app-HDR Boolean property unset while the
    /// cached value is false, so metadata is accepted as an equivalent strong
    /// signal. The release-compatible bootstrap is intentionally narrower:
    /// it requires an explicitly enabled experimental HDR launch and an HDR
    /// Gamescope output. Swapchain classification still limits it to 10-bit or
    /// float formats, so none of these signals can promote ordinary 8-bit SDR.
    [[nodiscard]] inline GamescopeHdrActivationDecision
    decideGamescopeHdrActivation(
            const GamescopeHdrActivationEvidence& evidence) {
        if (evidence.hdrExposureDisabled)
            return {.active = false, .source = "hdr-exposure-disabled"};
        if (evidence.appWantsHdr)
            return {
                .active = evidence.appWantsHdr,
                .source = "gamescope-app-colorspace",
            };
        if (evidence.appHdrMetadataPresent)
            return {.active = true, .source = "gamescope-app-hdr-metadata"};
        if (evidence.gamescopeDetected &&
                evidence.experimentalHdrRequested &&
                evidence.outputHdrEnabled.value_or(false)) {
            return {
                .active = true,
                .source = "experimental-hdr-output-bootstrap",
            };
        }
        return {};
    }

    /// Decide which feedback can safely select the colour pipeline before the
    /// first swapchain is created. Gamescope's per-application properties can
    /// still describe the previously held commit during process startup, so
    /// they retain the normal settling delay. The experimental bootstrap is
    /// different: it combines a process-local opt-in with the current output
    /// HDR state and can therefore initialize this process directly. This
    /// avoids creating an SDR passthrough context only to rebuild it moments
    /// later. The disabled-exposure path is also conclusively SDR.
    [[nodiscard]] inline std::optional<bool>
    initialGamescopeHdrActivation(
            const GamescopeHdrFeedbackSample& sample) {
        if (sample.status == "hdr-exposure-disabled")
            return sample.active;
        if (sample.activationSource ==
                "experimental-hdr-output-bootstrap" &&
                sample.active.value_or(false))
            return true;
        if (!sample.gamescopeDetected)
            return sample.active;
        return std::nullopt;
    }

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
