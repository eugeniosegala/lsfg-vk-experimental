/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace vk {
    /// Owns the short-lived arrays required by a legacy Vulkan queue submission.
    ///
    /// Most lsfg-vk submissions have at most two binary semaphores and one
    /// timeline semaphore. Keep that normal path entirely stack-backed while
    /// preserving an unbounded fallback for application-provided present waits.
    class SubmissionInfo {
    public:
        static constexpr size_t inlineSemaphoreCapacity{4};

        SubmissionInfo(std::span<const VkSemaphore> waitSemaphores,
            VkSemaphore waitTimelineSemaphore, uint64_t waitValue,
            std::span<const VkSemaphore> signalSemaphores,
            VkSemaphore signalTimelineSemaphore, uint64_t signalValue,
            VkCommandBuffer commandBuffer) : commandBuffer(commandBuffer) {
            this->waits.assign(waitSemaphores, waitTimelineSemaphore, waitValue, true);
            this->signals.assign(signalSemaphores, signalTimelineSemaphore, signalValue, false);

            this->timeline = {
                .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                .waitSemaphoreValueCount = static_cast<uint32_t>(this->waits.size()),
                .pWaitSemaphoreValues = this->waits.valuesData(),
                .signalSemaphoreValueCount = static_cast<uint32_t>(this->signals.size()),
                .pSignalSemaphoreValues = this->signals.valuesData()
            };
        }

        [[nodiscard]] VkSubmitInfo makeSubmitInfo() const {
            return {
                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .pNext = &this->timeline,
                .waitSemaphoreCount = static_cast<uint32_t>(this->waits.size()),
                .pWaitSemaphores = this->waits.semaphoresData(),
                .pWaitDstStageMask = this->waits.stagesData(),
                .commandBufferCount = 1,
                .pCommandBuffers = &this->commandBuffer,
                .signalSemaphoreCount = static_cast<uint32_t>(this->signals.size()),
                .pSignalSemaphores = this->signals.semaphoresData()
            };
        }

        [[nodiscard]] const VkTimelineSemaphoreSubmitInfo& timelineInfo() const {
            return this->timeline;
        }

    private:
        class SemaphorePayload {
        public:
            void assign(const std::span<const VkSemaphore> binarySemaphores,
                    const VkSemaphore timelineSemaphore, const uint64_t timelineValue,
                    const bool stagesRequired) {
                const size_t count = binarySemaphores.size() + (timelineSemaphore ? 1 : 0);
                if (count <= inlineSemaphoreCapacity) {
                    this->inlineCount = count;
                    std::copy(binarySemaphores.begin(), binarySemaphores.end(),
                        this->inlineSemaphores.begin());
                    std::fill_n(this->inlineValues.begin(), count, 0);
                    if (stagesRequired)
                        std::fill_n(this->inlineStages.begin(), count, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
                    if (timelineSemaphore) {
                        this->inlineSemaphores.at(count - 1) = timelineSemaphore;
                        this->inlineValues.at(count - 1) = timelineValue;
                    }
                    return;
                }

                this->overflowSemaphores.assign(binarySemaphores.begin(), binarySemaphores.end());
                if (timelineSemaphore)
                    this->overflowSemaphores.push_back(timelineSemaphore);
                this->overflowValues.assign(count, 0);
                if (timelineSemaphore)
                    this->overflowValues.back() = timelineValue;
                if (stagesRequired)
                    this->overflowStages.assign(count, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
            }

            [[nodiscard]] size_t size() const {
                return this->overflowSemaphores.empty()
                    ? this->inlineCount
                    : this->overflowSemaphores.size();
            }

            [[nodiscard]] const VkSemaphore* semaphoresData() const {
                return this->overflowSemaphores.empty()
                    ? this->inlineSemaphores.data()
                    : this->overflowSemaphores.data();
            }

            [[nodiscard]] const uint64_t* valuesData() const {
                return this->overflowSemaphores.empty()
                    ? this->inlineValues.data()
                    : this->overflowValues.data();
            }

            [[nodiscard]] const VkPipelineStageFlags* stagesData() const {
                return this->overflowSemaphores.empty()
                    ? this->inlineStages.data()
                    : this->overflowStages.data();
            }

        private:
            size_t inlineCount{0};
            std::array<VkSemaphore, inlineSemaphoreCapacity> inlineSemaphores{};
            std::array<uint64_t, inlineSemaphoreCapacity> inlineValues{};
            std::array<VkPipelineStageFlags, inlineSemaphoreCapacity> inlineStages{};
            std::vector<VkSemaphore> overflowSemaphores;
            std::vector<uint64_t> overflowValues;
            std::vector<VkPipelineStageFlags> overflowStages;
        };

        SemaphorePayload waits;
        SemaphorePayload signals;
        VkTimelineSemaphoreSubmitInfo timeline{};
        VkCommandBuffer commandBuffer{};
    };
}
