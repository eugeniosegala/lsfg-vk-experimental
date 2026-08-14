/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "instance.hpp"
#include "lsfg-vk-common/helpers/paths.hpp"
#include "swapchain.hpp"
#include "lsfg-vk-common/configuration/detection.hpp"
#include "lsfg-vk-common/helpers/errors.hpp"
#include "lsfg-vk-common/vulkan/vulkan.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <stdlib.h>
#include <vulkan/vulkan_core.h>

using namespace lsfgvk;
using namespace lsfgvk::layer;

#ifndef LSFGVK_BUILD_VERSION
#define LSFGVK_BUILD_VERSION "unknown"
#endif

namespace {
    constexpr char experimentalBuildIdentity[] =
        "lsfg-vk: experimental layer active; identity="
        "VK_LAYER_LSFGVK_experimental_frame_generation; build="
        LSFGVK_BUILD_VERSION;

    class ScopedEnvironmentOverride {
    public:
        ScopedEnvironmentOverride(std::string name, const char* value) :
                name(std::move(name)) {
            if (const char* current = std::getenv(this->name.c_str()))
                this->previousValue = current;

            if (setenv(this->name.c_str(), value, 1) != 0)
                throw ls::error("unable to set environment override: " + this->name);
        }

        ~ScopedEnvironmentOverride() {
            if (this->previousValue)
                static_cast<void>(setenv(
                    this->name.c_str(), this->previousValue->c_str(), 1
                ));
            else
                static_cast<void>(unsetenv(this->name.c_str()));
        }

        ScopedEnvironmentOverride(const ScopedEnvironmentOverride&) = delete;
        ScopedEnvironmentOverride& operator=(const ScopedEnvironmentOverride&) = delete;
        ScopedEnvironmentOverride(ScopedEnvironmentOverride&&) = delete;
        ScopedEnvironmentOverride& operator=(ScopedEnvironmentOverride&&) = delete;

    private:
        std::string name;
        std::optional<std::string> previousValue;
    };

    bool presentDiagnosticsEnabled() {
        const char* value = std::getenv("LSFGVK_PRESENT_DIAGNOSTICS");
        return value && std::string(value) != "0";
    }

    /// helper function to add required extensions
    std::vector<const char*> add_extensions(const char* const* existingExtensions, size_t count,
            const std::vector<const char*>& requiredExtensions) {
        std::vector<const char*> extensions(count);
        std::copy_n(existingExtensions, count, extensions.data());

        for (const auto& requiredExtension : requiredExtensions) {
            auto it = std::ranges::find_if(extensions,
                [requiredExtension](const char* extension) {
                    return std::string(extension) == std::string(requiredExtension);
                });
            if (it == extensions.end())
                extensions.push_back(requiredExtension);
        }

        return extensions;
    }
}

Root::Root() {
    std::cerr << experimentalBuildIdentity << '\n';

    const auto initialHdrFeedback =
        this->hdrFeedbackReader.diagnosticSample();
    this->lastHdrFeedbackSample = initialHdrFeedback.active;
    this->lastHdrFeedbackStatus = initialHdrFeedback.status;
    this->gamescopeHdrActive = this->lastHdrFeedbackSample;
    this->hdrFeedback.seed(this->lastHdrFeedbackSample);
    this->lastHdrFeedbackPoll = std::chrono::steady_clock::now();
    if (this->lastHdrFeedbackSample) {
        std::cerr << "lsfg-vk: Gamescope application HDR feedback initialized: active="
                  << *this->lastHdrFeedbackSample << '\n';
    } else {
        std::cerr << "lsfg-vk: Gamescope application HDR feedback unavailable; "
                     "normalized 10-bit swapchains remain SDR until confirmed; "
                  << "reason=" << initialHdrFeedback.status
                  << " display="
                  << (initialHdrFeedback.display.empty()
                        ? "(unset)" : initialHdrFeedback.display)
                  << '\n';
    }

    // find active profile
    const auto& profile = findProfile(this->config.get(), ls::identify());
    if (!profile.has_value())
        return;

    this->active_profile = profile->second;

    std::cerr << "lsfg-vk: using profile with name '" << this->active_profile->name << "' ";
    switch (profile->first) {
        case ls::IdentType::OVERRIDE:
            std::cerr << "(identified via override)\n";
            break;
        case ls::IdentType::EXECUTABLE:
            std::cerr << "(identified via executable)\n";
            break;
        case ls::IdentType::WINE_EXECUTABLE:
            std::cerr << "(identified via wine executable)\n";
            break;
        case ls::IdentType::PROCESS_NAME:
            std::cerr << "(identified via process name)\n";
            break;
    }
}

ConfigurationUpdateResult Root::update() {
    ConfigurationUpdateResult result;
    const auto now = std::chrono::steady_clock::now();
    constexpr auto hdrFeedbackPollInterval = std::chrono::milliseconds(250);
    if (!this->lastHdrFeedbackPoll ||
            now - *this->lastHdrFeedbackPoll >= hdrFeedbackPollInterval) {
        const auto hdrFeedbackSample =
            this->hdrFeedbackReader.diagnosticSample();
        this->lastHdrFeedbackSample = hdrFeedbackSample.active;
        if (hdrFeedbackSample.status != this->lastHdrFeedbackStatus) {
            this->lastHdrFeedbackStatus = hdrFeedbackSample.status;
            std::cerr << "lsfg-vk: Gamescope application HDR feedback status: "
                      << hdrFeedbackSample.status
                      << "; display="
                      << (hdrFeedbackSample.display.empty()
                            ? "(unset)" : hdrFeedbackSample.display)
                      << '\n';
        }
        this->lastHdrFeedbackPoll = now;
    }

    if (const auto changed = this->hdrFeedback.observe(
            this->lastHdrFeedbackSample, now)) {
        this->gamescopeHdrActive = changed;
        this->runtimeStateRevision++;
        result.hdrFeedbackChanged = true;
        for (auto& [swapchain, context] : this->swapchains) {
            static_cast<void>(swapchain);
            if (context.updateGamescopeHdrState(
                    *changed, this->runtimeStateRevision))
                result.hdrContextsDeferred++;
        }
        std::cerr << "lsfg-vk: Gamescope application HDR feedback stabilized: active="
                  << *changed
                  << "; contexts_pending_recreation="
                  << result.hdrContextsDeferred << '\n';
    }

    const auto previousGlobal = this->config.get().global();
    if (!this->config.update())
        return result;

    result.reloaded = true;
    this->runtimeStateRevision++;
    const auto& currentGlobal = this->config.get().global();
    result.globalChangeDeferred =
        previousGlobal.dll != currentGlobal.dll ||
        previousGlobal.allow_fp16 != currentGlobal.allow_fp16;

    const auto& profile = findProfile(this->config.get(), ls::identify());
    if (profile.has_value())
        this->active_profile = profile->second;
    else
        this->active_profile = std::nullopt;

    if (this->active_profile) {
        for (auto& [swapchain, context] : this->swapchains) {
            static_cast<void>(swapchain);
            switch (context.updateProfile(
                    *this->active_profile, this->runtimeStateRevision)) {
                case ProfileUpdateAction::NoRuntimeChange:
                    break;
                case ProfileUpdateAction::ApplyLive:
                    result.liveContextsUpdated++;
                    break;
                case ProfileUpdateAction::DeferUntilSwapchainRecreation:
                    result.deferredContexts++;
                    break;
            }
        }
    } else {
        // Losing the active profile must stop generation immediately, but it
        // does not require destroying any in-flight Vulkan resources.
        for (auto& [swapchain, context] : this->swapchains) {
            static_cast<void>(swapchain);
            context.disableFrameGeneration();
            result.liveContextsUpdated++;
        }
    }

    return result;
}

void Root::modifyInstanceCreateInfo(VkInstanceCreateInfo& createInfo,
        const std::function<void(void)>& finish) const {
    if (!this->active_profile.has_value())
        return;

    auto extensions = add_extensions(
        createInfo.ppEnabledExtensionNames,
        createInfo.enabledExtensionCount,
        {
            "VK_KHR_get_physical_device_properties2",
            "VK_KHR_external_memory_capabilities",
            "VK_KHR_external_semaphore_capabilities"
        }
    );
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

    finish();
}

void Root::modifyDeviceCreateInfo(VkDeviceCreateInfo& createInfo,
        const std::function<void(void)>& finish) const {
    if (!this->active_profile.has_value())
        return;

    auto extensions = add_extensions(
        createInfo.ppEnabledExtensionNames,
        createInfo.enabledExtensionCount,
        {
            "VK_KHR_external_memory",
            "VK_KHR_external_memory_fd",
            "VK_KHR_external_semaphore",
            "VK_KHR_external_semaphore_fd",
            "VK_KHR_timeline_semaphore"
        }
    );
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

    bool isFeatureEnabled = false;
    auto* featureInfo = reinterpret_cast<VkBaseInStructure*>(const_cast<void*>(createInfo.pNext));
    while (featureInfo) {
        if (featureInfo->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
            auto* features = reinterpret_cast<VkPhysicalDeviceVulkan12Features*>(featureInfo);
            features->timelineSemaphore = VK_TRUE;
            isFeatureEnabled = true;
        } else if (featureInfo->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES) {
            auto* features = reinterpret_cast<VkPhysicalDeviceTimelineSemaphoreFeatures*>(featureInfo);
            features->timelineSemaphore = VK_TRUE;
            isFeatureEnabled = true;
        }

        featureInfo = const_cast<VkBaseInStructure*>(featureInfo->pNext);
    }

    VkPhysicalDeviceTimelineSemaphoreFeatures timelineFeatures{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
        .pNext = const_cast<void*>(createInfo.pNext),
        .timelineSemaphore = VK_TRUE
    };
    if (!isFeatureEnabled)
        createInfo.pNext = &timelineFeatures;

    finish();
}

void Root::modifySwapchainCreateInfo(const vk::Vulkan& vk, VkSwapchainCreateInfoKHR& createInfo,
        const std::function<void(void)>& finish) const {
    if (!this->active_profile.has_value())
        return;

    VkSurfaceCapabilitiesKHR caps{}; // NOLINT (enum value 0)
    auto res = vk.fi().GetPhysicalDeviceSurfaceCapabilitiesKHR(
        vk.physdev(), createInfo.surface, &caps);
    if (res != VK_SUCCESS)
        throw ls::vulkan_error(res, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR() failed");

    context_ModifySwapchainCreateInfo(
        *this->active_profile,
        caps.maxImageCount,
        createInfo,
        this->gamescopeHdrActive.value_or(false)
    );

    finish();
}

void Root::createSwapchainContext(const vk::Vulkan& vk,
        VkSwapchainKHR swapchain, const SwapchainInfo& info) {
    if (!this->active_profile.has_value())
        throw ls::error("attempted to create swapchain context while layer is inactive");
    const auto& profile = *this->active_profile;

    if (!this->backend.has_value()) { // emplace backend late, due to loader bug
        const auto& global = this->config.get().global();

        try {
            // The backend owns a separate Vulkan instance. Prevent this
            // experimental layer and either public LSFG identity from entering
            // that internal instance, while preserving caller-provided values
            // for the rest of the game process.
            const ScopedEnvironmentOverride disableExperimental(
                "DISABLE_LSFGVK_EXPERIMENTAL", "1"
            );
            const ScopedEnvironmentOverride disablePublic(
                "DISABLE_LSFGVK", "1"
            );
            const ScopedEnvironmentOverride disableLegacy(
                "DISABLE_LSFG", "1"
            );

            std::string dll{};
            if (global.dll.has_value())
                dll = *global.dll;
            else
                dll = ls::findShaderDll();

            this->backend.emplace(
                [gpu = profile.gpu](
                    const std::string& deviceName,
                    std::pair<const std::string&, const std::string&> ids,
                    const std::optional<std::string>& pci
                ) {
                    if (!gpu)
                        return true;

                    return (deviceName == *gpu)
                        || (ids.first + ":" + ids.second == *gpu)
                        || (pci && *pci == *gpu);
                },
                dll, global.allow_fp16
            );
        } catch (const std::exception& e) {
            throw ls::error("failed to create backend instance", e);
        }
    }

    const bool inserted = this->swapchains.emplace(swapchain,
        Swapchain(vk, this->backend.mut(), profile, info,
            this->gamescopeHdrActive.value_or(false),
            this->runtimeStateRevision)).second;
    const auto insertedContext = this->swapchains.find(swapchain);
    const uint64_t diagnosticsContextId =
        insertedContext != this->swapchains.end()
        ? insertedContext->second.diagnosticsId()
        : 0;

    if (presentDiagnosticsEnabled()) {
        std::cerr << "lsfg-vk: present diagnostics: operation=swapchain-context-create"
                  << " context=" << diagnosticsContextId
                  << " swapchain=" << swapchain
                  << " active_contexts=" << this->swapchains.size()
                  << " inserted=" << inserted
                  << " layer_forced_recreation=disabled"
                  << '\n';
    }
}

void Root::removeSwapchainContext(VkSwapchainKHR swapchain) {
    const auto context = this->swapchains.find(swapchain);
    const uint64_t diagnosticsContextId = context != this->swapchains.end()
        ? context->second.diagnosticsId()
        : 0;
    const size_t removed = this->swapchains.erase(swapchain);
    if (presentDiagnosticsEnabled()) {
        std::cerr << "lsfg-vk: present diagnostics: operation=swapchain-context-destroy"
                  << " context=" << diagnosticsContextId
                  << " swapchain=" << swapchain
                  << " active_contexts=" << this->swapchains.size()
                  << " removed=" << removed << '\n';
    }
}
