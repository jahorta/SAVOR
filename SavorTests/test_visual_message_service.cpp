#include "Runner/Runtime/Services/Visual/SessionVisualMessageService.h"
#include "Runner/Runtime/ProgramKind.h"

#include "gtest/gtest.h"

#include <string>
#include <utility>
#include <vector>

namespace savor::runtime {
namespace {

class RecordingVisualBackend final : public IVisualMessageBackendPort
{
public:
    bool available = true;
    std::vector<std::pair<VisualMessageSlot, std::string>> messages;

    [[nodiscard]] bool IsAvailable() const noexcept override
    {
        return available;
    }

    BackendResult ReplaceMessage(
        VisualMessageSlot slot,
        std::string message) override
    {
        messages.emplace_back(slot, std::move(message));
        return BackendResult::Success();
    }
};

TEST(SessionVisualMessageService, ReplacesCurrentPhaseAndReturnsToIdle)
{
    RecordingVisualBackend backend;
    SessionVisualMessageService service(
        backend,
        {.show_current_phase = true});

    EXPECT_TRUE(service.SetIdle().ok);
    EXPECT_TRUE(service.SetCurrentPhase("TAS Movie Cutscene").ok);
    EXPECT_TRUE(service.SetCurrentPhase("TAS Movie Cutscene").ok);
    EXPECT_TRUE(service.SetIdle().ok);

    ASSERT_EQ(backend.messages.size(), 3u);
    EXPECT_EQ(backend.messages[0].second, "Current phase: Idle");
    EXPECT_EQ(backend.messages[1].second,
              "Current phase: TAS Movie Cutscene");
    EXPECT_EQ(backend.messages[2].second, "Current phase: Idle");
}

TEST(SessionVisualMessageService, DisabledOrUnavailableBackendIsANoOp)
{
    RecordingVisualBackend disabled_backend;
    SessionVisualMessageService disabled(
        disabled_backend,
        {.show_current_phase = false});
    EXPECT_TRUE(disabled.SetCurrentPhase("Battle Completion").ok);
    EXPECT_TRUE(disabled_backend.messages.empty());

    RecordingVisualBackend unavailable_backend;
    unavailable_backend.available = false;
    SessionVisualMessageService unavailable(
        unavailable_backend,
        {.show_current_phase = true});
    EXPECT_TRUE(unavailable.SetCurrentPhase("Battle Completion").ok);
    EXPECT_TRUE(unavailable_backend.messages.empty());
}

TEST(SessionVisualMessageService, ProgramKindsHaveSharedFriendlyNames)
{
    EXPECT_EQ(ProgramKindDisplayName(PK_TasMovieCutscene),
              "TAS Movie Cutscene");
    EXPECT_EQ(ProgramKindDisplayName(PK_BattleSingleTurnRunner),
              "Battle Single Turn");
    EXPECT_TRUE(ProgramKindDisplayName(999).empty());
}

} // namespace
} // namespace savor::runtime
