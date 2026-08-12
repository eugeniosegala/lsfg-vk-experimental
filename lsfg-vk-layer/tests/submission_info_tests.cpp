/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "lsfg-vk-common/vulkan/submission_info.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace {
    struct TestFailure {
        std::string message;
    };

    void require(const bool condition, std::string message) {
        if (!condition)
            throw TestFailure{std::move(message)};
    }

    template <typename Handle>
    Handle fakeHandle(const uintptr_t value) {
        if constexpr (std::is_pointer_v<Handle>)
            return reinterpret_cast<Handle>(value);
        else
            return static_cast<Handle>(value);
    }

    void verifySubmission(const VkSubmitInfo& submitInfo,
            const VkTimelineSemaphoreSubmitInfo& timelineInfo,
            const uint32_t waitCount, const uint32_t signalCount) {
        require(submitInfo.sType == VK_STRUCTURE_TYPE_SUBMIT_INFO,
            "submit info must retain its structure type");
        require(submitInfo.pNext == &timelineInfo,
            "submit info must retain its timeline payload");
        require(submitInfo.waitSemaphoreCount == waitCount,
            "wait semaphore count changed");
        require(submitInfo.signalSemaphoreCount == signalCount,
            "signal semaphore count changed");
        require(timelineInfo.waitSemaphoreValueCount == waitCount,
            "wait timeline value count must match wait semaphores");
        require(timelineInfo.signalSemaphoreValueCount == signalCount,
            "signal timeline value count must match signal semaphores");
    }

    void testTimelineOnlySubmission() {
        const vk::SubmissionInfo submission{{},
            fakeHandle<VkSemaphore>(0x10), 17,
            {}, fakeHandle<VkSemaphore>(0x20), 23,
            fakeHandle<VkCommandBuffer>(0x30)};
        const VkCommandBuffer commandBuffer = fakeHandle<VkCommandBuffer>(0x30);
        const VkSubmitInfo submitInfo = submission.makeSubmitInfo();
        const auto& timeline = submission.timelineInfo();

        verifySubmission(submitInfo, timeline, 1, 1);
        require(submitInfo.pWaitSemaphores[0] == fakeHandle<VkSemaphore>(0x10),
            "timeline wait must be appended to the wait list");
        require(submitInfo.pSignalSemaphores[0] == fakeHandle<VkSemaphore>(0x20),
            "timeline signal must be appended to the signal list");
        require(timeline.pWaitSemaphoreValues[0] == 17,
            "timeline wait value changed");
        require(timeline.pSignalSemaphoreValues[0] == 23,
            "timeline signal value changed");
        require(submitInfo.pWaitDstStageMask[0] == VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            "wait stage mask changed");
        require(submitInfo.pCommandBuffers[0] == commandBuffer,
            "command buffer changed");
    }

    void testBinaryAndTimelineOrdering() {
        const std::array<VkSemaphore, 2> waits{
            fakeHandle<VkSemaphore>(0x11), fakeHandle<VkSemaphore>(0x12)
        };
        const std::array<VkSemaphore, 2> signals{
            fakeHandle<VkSemaphore>(0x21), fakeHandle<VkSemaphore>(0x22)
        };
        const vk::SubmissionInfo submission{waits,
            fakeHandle<VkSemaphore>(0x13), 41,
            signals, fakeHandle<VkSemaphore>(0x23), 43,
            fakeHandle<VkCommandBuffer>(0x31)};
        const VkSubmitInfo submitInfo = submission.makeSubmitInfo();
        const auto& timeline = submission.timelineInfo();

        verifySubmission(submitInfo, timeline, 3, 3);
        require(submitInfo.pWaitSemaphores[0] == waits[0] &&
                submitInfo.pWaitSemaphores[1] == waits[1] &&
                submitInfo.pWaitSemaphores[2] == fakeHandle<VkSemaphore>(0x13),
            "binary waits must precede the timeline wait");
        require(submitInfo.pSignalSemaphores[0] == signals[0] &&
                submitInfo.pSignalSemaphores[1] == signals[1] &&
                submitInfo.pSignalSemaphores[2] == fakeHandle<VkSemaphore>(0x23),
            "binary signals must precede the timeline signal");
        require(timeline.pWaitSemaphoreValues[0] == 0 &&
                timeline.pWaitSemaphoreValues[1] == 0 &&
                timeline.pWaitSemaphoreValues[2] == 41,
            "only the timeline wait may carry a non-zero value");
        require(timeline.pSignalSemaphoreValues[0] == 0 &&
                timeline.pSignalSemaphoreValues[1] == 0 &&
                timeline.pSignalSemaphoreValues[2] == 43,
            "only the timeline signal may carry a non-zero value");
    }

    void testOverflowPreservesApplicationWaits() {
        const std::array<VkSemaphore, 5> waits{
            fakeHandle<VkSemaphore>(0x41), fakeHandle<VkSemaphore>(0x42),
            fakeHandle<VkSemaphore>(0x43), fakeHandle<VkSemaphore>(0x44),
            fakeHandle<VkSemaphore>(0x45)
        };
        const vk::SubmissionInfo submission{waits,
            fakeHandle<VkSemaphore>(0x46), 59,
            {}, VK_NULL_HANDLE, 0,
            fakeHandle<VkCommandBuffer>(0x50)};
        const VkSubmitInfo submitInfo = submission.makeSubmitInfo();
        const auto& timeline = submission.timelineInfo();

        verifySubmission(submitInfo, timeline, 6, 0);
        for (size_t i = 0; i < waits.size(); ++i) {
            require(submitInfo.pWaitSemaphores[i] == waits[i],
                "overflow path reordered an application wait semaphore");
            require(timeline.pWaitSemaphoreValues[i] == 0,
                "binary application waits must retain a zero timeline value");
            require(submitInfo.pWaitDstStageMask[i] == VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                "overflow path changed a wait stage mask");
        }
        require(submitInfo.pWaitSemaphores[waits.size()] == fakeHandle<VkSemaphore>(0x46),
            "overflow path lost the timeline wait");
        require(timeline.pWaitSemaphoreValues[waits.size()] == 59,
            "overflow path changed the timeline wait value");
    }
}

int main() {
    try {
        testTimelineOnlySubmission();
        testBinaryAndTimelineOrdering();
        testOverflowPreservesApplicationWaits();
    } catch (const TestFailure& failure) {
        std::cerr << "submission-info test failed: " << failure.message << '\n';
        return 1;
    }

    std::cout << "submission-info tests passed\n";
    return 0;
}
