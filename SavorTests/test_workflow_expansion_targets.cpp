#include <gtest/gtest.h>

#include "Execution/Workflow/WorkflowExpansionService.h"

namespace {
using savor::db::execution::workflow::NormalizeFirstBattleExpansionTargets;
using savor::db::execution::workflow::WorkflowExpansionTarget;

TEST(WorkflowExpansionTargets, NormalizesEveryRtcToACompleteDelayPrefix) {
    const auto targets = NormalizeFirstBattleExpansionTargets({
        {.neutral_epoch_count=3, .rtc_value=355},
        {.neutral_epoch_count=1, .rtc_value=356},
    });
    const std::vector<WorkflowExpansionTarget> expected{
        {0,355}, {0,356}, {1,355}, {1,356}, {2,355}, {3,355},
    };
    EXPECT_EQ(targets, expected);
}

TEST(WorkflowExpansionTargets, DeduplicatesAndRejectsInvalidCoordinates) {
    const auto targets = NormalizeFirstBattleExpansionTargets({
        {.neutral_epoch_count=1, .rtc_value=9},
        {.neutral_epoch_count=1, .rtc_value=9},
        {.neutral_epoch_count=-1, .rtc_value=9},
        {.neutral_epoch_count=2, .rtc_value=-1},
    });
    const std::vector<WorkflowExpansionTarget> expected{{0,9}, {1,9}};
    EXPECT_EQ(targets, expected);
}
}
