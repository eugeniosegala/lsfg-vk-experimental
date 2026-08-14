/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "gamescope_hdr_feedback.hpp"

#include <chrono>
#include <filesystem>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <dlfcn.h>
#endif

using namespace lsfgvk::layer;

namespace {
    bool environmentFlagEnabled(const char* value) {
        if (!value)
            return false;
        const std::string_view flag(value);
        return flag == "1" || flag == "true" || flag == "yes" || flag == "on";
    }

    bool gamescopeHdrFeedbackMayChange() {
        const char* display = std::getenv("DISPLAY");
        return display && *display;
    }

    constexpr char gamescopePidProperty[] = "GAMESCOPE_PID";
    constexpr char gamescopeServerIdProperty[] = "GAMESCOPE_XWAYLAND_SERVER_ID";
    constexpr char gamescopeHdrProperty[] =
        "GAMESCOPE_COLOR_APP_WANTS_HDR_FEEDBACK";
    constexpr char gamescopeRefreshProperty[] =
        "GAMESCOPE_DISPLAY_REFRESH_RATE_FEEDBACK";
}

struct GamescopeHdrFeedbackReader::Impl {
    std::mutex sampleMutex;
    GamescopeHdrFeedbackSample latestSample;
    std::jthread monitor;

#if defined(__linux__)
    void* library{nullptr};
    Display* display{nullptr};
    Atom feedbackAtom{None};
    Window root{None};
    std::string selectedDisplayName;
    decltype(&XOpenDisplay) openDisplay{nullptr};
    decltype(&XCloseDisplay) closeDisplay{nullptr};
    decltype(&XInternAtom) internAtom{nullptr};
    decltype(&XDefaultRootWindow) defaultRootWindow{nullptr};
    decltype(&XGetWindowProperty) getWindowProperty{nullptr};
    decltype(&XFree) freeData{nullptr};

    template<typename Function>
    bool resolve(Function& function, const char* name) {
        function = reinterpret_cast<Function>(dlsym(this->library, name));
        return function != nullptr;
    }

    std::optional<uint32_t> readCardinal(Display* sourceDisplay,
            const Window sourceRoot, const char* propertyName) {
        const Atom property = this->internAtom(
            sourceDisplay, propertyName, True
        );
        if (property == None)
            return std::nullopt;

        Atom actualType{None};
        int actualFormat{};
        unsigned long itemCount{};
        unsigned long bytesAfter{};
        unsigned char* data{nullptr};
        const int result = this->getWindowProperty(
            sourceDisplay, sourceRoot, property, 0, 1, False, XA_CARDINAL,
            &actualType, &actualFormat, &itemCount, &bytesAfter, &data
        );
        std::optional<uint32_t> value;
        if (result == Success && actualType == XA_CARDINAL &&
                actualFormat == 32 && itemCount == 1 && data) {
            value = static_cast<uint32_t>(
                *reinterpret_cast<const unsigned long*>(data)
            );
        }
        if (data)
            this->freeData(data);
        return value;
    }

    GamescopeXwaylandDisplay identifyDisplay(
            const std::string& name, Display* candidateDisplay) {
        const Window candidateRoot = this->defaultRootWindow(candidateDisplay);
        return {
            .display = name,
            .gamescopePid = this->readCardinal(
                candidateDisplay, candidateRoot, gamescopePidProperty
            ),
            .serverId = this->readCardinal(
                candidateDisplay, candidateRoot, gamescopeServerIdProperty
            ),
        };
    }

    std::vector<std::string> localDisplayCandidates() {
        std::vector<std::string> candidates;
        std::error_code error;
        const std::filesystem::path socketDirectory{"/tmp/.X11-unix"};
        for (std::filesystem::directory_iterator it(socketDirectory, error), end;
                !error && it != end; it.increment(error)) {
            const std::string filename = it->path().filename().string();
            if (filename.size() <= 1 || filename.front() != 'X' ||
                    !std::ranges::all_of(filename.substr(1),
                        [](const char value) { return value >= '0' && value <= '9'; }))
                continue;
            candidates.emplace_back(":" + filename.substr(1));
        }
        std::ranges::sort(candidates);
        candidates.erase(std::unique(candidates.begin(), candidates.end()),
            candidates.end());
        return candidates;
    }

    bool initialize() {
        if (this->display)
            return this->root != None;

        if (!this->library)
            this->library = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
        if (!this->library)
            return false;

        if (!this->openDisplay &&
                (!this->resolve(this->openDisplay, "XOpenDisplay") ||
                    !this->resolve(this->closeDisplay, "XCloseDisplay") ||
                    !this->resolve(this->internAtom, "XInternAtom") ||
                    !this->resolve(this->defaultRootWindow, "XDefaultRootWindow") ||
                    !this->resolve(this->getWindowProperty, "XGetWindowProperty") ||
                    !this->resolve(this->freeData, "XFree")))
            return false;

        Display* currentDisplay = this->openDisplay(nullptr);
        if (!currentDisplay)
            return false;

        const char* currentNameValue = std::getenv("DISPLAY");
        const std::string currentName = currentNameValue ? currentNameValue : "";
        const auto currentIdentity = this->identifyDisplay(
            currentName, currentDisplay
        );

        std::vector<GamescopeXwaylandDisplay> candidateIdentities;
        std::vector<std::pair<std::string, Display*>> candidateConnections;
        if (currentIdentity.gamescopePid && currentIdentity.serverId &&
                *currentIdentity.serverId != 0) {
            for (const auto& candidateName : this->localDisplayCandidates()) {
                if (candidateName == currentName)
                    continue;
                Display* candidateDisplay = this->openDisplay(candidateName.c_str());
                if (!candidateDisplay)
                    continue;
                candidateIdentities.push_back(this->identifyDisplay(
                    candidateName, candidateDisplay
                ));
                candidateConnections.emplace_back(candidateName, candidateDisplay);
            }
        }

        const auto rootDisplayName = selectGamescopeRootDisplay(
            currentIdentity, candidateIdentities
        );
        if (rootDisplayName && *rootDisplayName != currentName) {
            const auto connection = std::ranges::find_if(candidateConnections,
                [&rootDisplayName](const auto& value) {
                    return value.first == *rootDisplayName;
                });
            if (connection != candidateConnections.end()) {
                this->display = connection->second;
                connection->second = nullptr;
                this->closeDisplay(currentDisplay);
            }
        }
        if (!this->display)
            this->display = currentDisplay;
        for (const auto& [name, connection] : candidateConnections) {
            static_cast<void>(name);
            if (connection)
                this->closeDisplay(connection);
        }

        this->root = this->defaultRootWindow(this->display);
        const auto selectedIdentity = this->identifyDisplay(
            rootDisplayName.value_or(currentName), this->display
        );
        this->selectedDisplayName = selectedIdentity.display;
        this->feedbackAtom = this->internAtom(
            this->display, gamescopeHdrProperty, True
        );
        return this->root != None;
    }

    GamescopeHdrFeedbackSample sampleOnce() {
        const char* displayName = std::getenv("DISPLAY");
        GamescopeHdrFeedbackSample sample{.display = displayName ? displayName : ""};
        if (!displayName || !*displayName) {
            sample.status = "display-environment-missing";
            return sample;
        }

        // DXVK_HDR is an exposure/capability signal, not an active-HDR
        // signal. A false capability does conclusively mean the game is SDR,
        // but still resolve Gamescope first: its identity, refresh budget and
        // WSI ownership remain relevant to SDR presentation.
        const char* dxvkHdr = std::getenv("DXVK_HDR");
        const bool hdrExposureDisabled =
            dxvkHdr && !environmentFlagEnabled(dxvkHdr);

        if (!this->initialize()) {
            sample.status = this->library
                ? "x11-display-open-failed"
                : "x11-library-unavailable";
            return sample;
        }

        sample.display = this->selectedDisplayName;
        sample.gamescopePid = this->readCardinal(
            this->display, this->root, gamescopePidProperty
        );
        sample.xwaylandServerId = this->readCardinal(
            this->display, this->root, gamescopeServerIdProperty
        );
        sample.gamescopeDetected = sample.gamescopePid.has_value() &&
            sample.xwaylandServerId.has_value();
        sample.refreshHz = this->readCardinal(
            this->display, this->root, gamescopeRefreshProperty
        );

        if (hdrExposureDisabled) {
            sample.active = false;
            sample.status = "hdr-exposure-disabled";
            return sample;
        }

        if (this->feedbackAtom == None) {
            this->feedbackAtom = this->internAtom(
                this->display,
                gamescopeHdrProperty,
                True
            );
            if (this->feedbackAtom == None) {
                sample.status = "feedback-atom-unavailable";
                return sample;
            }
        }

        Atom actualType{None};
        int actualFormat{};
        unsigned long itemCount{};
        unsigned long bytesAfter{};
        unsigned char* data{nullptr};
        const int result = this->getWindowProperty(
            this->display,
            this->root,
            this->feedbackAtom,
            0,
            1,
            False,
            XA_CARDINAL,
            &actualType,
            &actualFormat,
            &itemCount,
            &bytesAfter,
            &data
        );

        if (result == Success && actualType == XA_CARDINAL &&
                actualFormat == 32 && itemCount == 1 && data) {
            const auto raw = *reinterpret_cast<const unsigned long*>(data);
            sample.active = raw != 0;
            sample.status = "confirmed";
        } else if (result != Success) {
            sample.status = "property-read-failed";
        } else if (actualType == None || itemCount == 0) {
            sample.status = "feedback-property-unset";
        } else {
            sample.status = "feedback-property-invalid";
        }
        if (data)
            this->freeData(data);
        return sample;
    }
#else
    GamescopeHdrFeedbackSample sampleOnce() {
        return {
            .status = "unsupported-platform",
        };
    }
#endif

    void refresh() {
        const auto sample = this->sampleOnce();
        std::scoped_lock lock(this->sampleMutex);
        this->latestSample = sample;
    }

    void start() {
        // One synchronous startup read lets the first swapchain use the right
        // colour pipeline. All later X11 round trips stay off the presentation
        // thread.
        this->refresh();
        if (!gamescopeHdrFeedbackMayChange())
            return;

        this->monitor = std::jthread([this](const std::stop_token stop) {
            while (!stop.stop_requested()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                if (stop.stop_requested())
                    break;
                this->refresh();
            }
        });
    }

    ~Impl() {
        if (this->monitor.joinable()) {
            this->monitor.request_stop();
            this->monitor.join();
        }
#if defined(__linux__)
        if (this->display && this->closeDisplay)
            this->closeDisplay(this->display);
        if (this->library)
            dlclose(this->library);
#endif
    }
};

GamescopeHdrFeedbackReader::GamescopeHdrFeedbackReader() :
    impl(std::make_unique<Impl>()) {
    this->impl->start();
}

GamescopeHdrFeedbackReader::~GamescopeHdrFeedbackReader() = default;
GamescopeHdrFeedbackReader::GamescopeHdrFeedbackReader(
    GamescopeHdrFeedbackReader&&) noexcept = default;
GamescopeHdrFeedbackReader& GamescopeHdrFeedbackReader::operator=(
    GamescopeHdrFeedbackReader&&) noexcept = default;

std::optional<bool> GamescopeHdrFeedbackReader::sample() const {
    std::scoped_lock lock(this->impl->sampleMutex);
    return this->impl->latestSample.active;
}

GamescopeHdrFeedbackSample
GamescopeHdrFeedbackReader::diagnosticSample() const {
    std::scoped_lock lock(this->impl->sampleMutex);
    return this->impl->latestSample;
}
