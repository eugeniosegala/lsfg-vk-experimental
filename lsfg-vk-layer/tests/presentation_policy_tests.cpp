/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "presentation_policy.hpp"

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
    expect(generatedImageDeadlineNs(120, std::nullopt, 2, 50'000'000) ==
            6'249'999,
        "120 Hz generated-image deadline should retain present headroom");
    expect(generatedImageDeadlineNs(60, std::nullopt, 2, 50'000'000) ==
            12'000'000,
        "generated-image deadline must keep its anti-hitch upper bound");

    FixedRefreshBudget budget;
    const auto start = FixedRefreshBudget::TimePoint{};
    size_t generated = 0;
    for (size_t frame = 0; frame <= 630; ++frame) {
        generated += budget.plan(
            start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(static_cast<double>(frame) / 63.0)
            ),
            120,
            1
        );
    }
    // The first real frame is deliberately ungenerated while timing warms up.
    expect(generated >= 565 && generated <= 575,
        "63 FPS Fixed 2x should budget approximately 120 displayed FPS");

    budget.reset();
    generated = 0;
    for (size_t frame = 0; frame <= 600; ++frame) {
        generated += budget.plan(start + frame * 10ms, 120, 1);
    }
    expect(generated >= 115 && generated <= 125,
        "100 FPS Fixed 2x should synthesize only the displayable remainder");

    std::cout << "presentation policy tests passed\n";
    return 0;
}
