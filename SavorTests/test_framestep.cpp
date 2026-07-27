#include <gtest/gtest.h>

#include "Runner/Runtime/EmulationSession.h"
#include "Runner/Runtime/IDolphinBackend.h"

#include <chrono>

namespace {

template <typename T>
concept HasPublicPause = requires(T& value) {
    value.Pause(std::chrono::milliseconds{1});
};

template <typename T>
concept HasPublicResume = requires(T& value) {
    value.Resume();
};

template <typename T>
concept HasPublicInstructionStep = requires(T& value) {
    value.StepInstruction(std::chrono::milliseconds{1});
};

template <typename T>
concept HasPublicFrameStep = requires(T& value) {
    value.StepFrame(std::chrono::milliseconds{1});
};

static_assert(!HasPublicPause<savor::runtime::IDolphinBackend>);
static_assert(!HasPublicResume<savor::runtime::IDolphinBackend>);
static_assert(!HasPublicInstructionStep<savor::runtime::IDolphinBackend>);
static_assert(!HasPublicFrameStep<savor::runtime::IDolphinBackend>);
static_assert(!HasPublicPause<savor::runtime::EmulationSession>);
static_assert(!HasPublicResume<savor::runtime::EmulationSession>);
static_assert(!HasPublicInstructionStep<savor::runtime::EmulationSession>);
static_assert(!HasPublicFrameStep<savor::runtime::EmulationSession>);

TEST(LegacyFrameStep, DirectAdvancementEscapesAreNotPublic)
{
    SUCCEED();
}

} // namespace
