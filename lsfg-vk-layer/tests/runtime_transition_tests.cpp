/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "runtime_transition.hpp"
#include "gamescope_hdr_feedback.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string_view>

using namespace lsfgvk::layer;
using namespace std::chrono_literals;

namespace {
    void expect(const bool condition, const std::string_view message) {
        if (condition)
            return;
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }

}

int main() {
    const auto start = StableBooleanFeedback::TimePoint{};

    StableBooleanFeedback feedback(750ms);
    feedback.seed(false);
    expect(feedback.value() == false, "The initial Gamescope feedback should be seeded");
    expect(!feedback.observe(true, start),
        "A single HDR feedback sample must not change the confirmed state");
    expect(!feedback.observe(false, start + 100ms),
        "A transient HDR feedback sample should be cancelled by the old state");
    expect(!feedback.observe(true, start + 200ms),
        "A new HDR candidate should start a fresh stability window");
    expect(!feedback.observe(true, start + 949ms),
        "HDR feedback must remain pending until the whole stability window passes");
    const auto hdrEnabled = feedback.observe(true, start + 950ms);
    expect(hdrEnabled && *hdrEnabled,
        "Stable Gamescope feedback should confirm active HDR");
    expect(!feedback.observe(true, start + 2s),
        "Repeated confirmed feedback must not produce another transition");

    const GamescopeXwaylandDisplay gameDisplay{
        .display = ":1", .gamescopePid = 42, .serverId = 1,
    };
    const std::vector<GamescopeXwaylandDisplay> displays{
        {.display = ":2", .gamescopePid = 99, .serverId = 0},
        {.display = ":0", .gamescopePid = 42, .serverId = 0},
    };
    expect(selectGamescopeRootDisplay(gameDisplay, displays) == ":0",
        "HDR feedback must resolve Gamescope server zero for the same process");
    expect(!selectGamescopeRootDisplay(
            {.display = ":8"}, displays),
        "an unrelated X11 display must not be guessed as Gamescope root");

    std::cout << "runtime transition tests passed\n";
    return 0;
}
