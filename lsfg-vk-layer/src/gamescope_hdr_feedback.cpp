/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "gamescope_hdr_feedback.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>

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

        this->display = this->openDisplay(nullptr);
        if (!this->display)
            return false;

        this->root = this->defaultRootWindow(this->display);
        this->feedbackAtom = this->internAtom(
            this->display, "GAMESCOPE_COLOR_APP_WANTS_HDR_FEEDBACK", True
        );
        return this->root != None;
    }

    GamescopeHdrFeedbackSample sampleOnce() {
        const char* displayName = std::getenv("DISPLAY");
        GamescopeHdrFeedbackSample sample{
            .display = displayName ? displayName : "",
        };
        if (!displayName || !*displayName) {
            sample.status = "display-environment-missing";
            return sample;
        }

        // DXVK_HDR is an exposure/capability signal, not an active-HDR
        // signal. A false capability does, however, conclusively mean the
        // game is SDR.
        const char* dxvkHdr = std::getenv("DXVK_HDR");
        if (dxvkHdr && !environmentFlagEnabled(dxvkHdr)) {
            sample.active = false;
            sample.status = "hdr-exposure-disabled";
            return sample;
        }

        if (!this->initialize()) {
            sample.status = this->library
                ? "x11-display-open-failed"
                : "x11-library-unavailable";
            return sample;
        }

        if (this->feedbackAtom == None) {
            this->feedbackAtom = this->internAtom(
                this->display,
                "GAMESCOPE_COLOR_APP_WANTS_HDR_FEEDBACK",
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
