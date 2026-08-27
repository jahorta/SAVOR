#include <gtest/gtest.h>

#include "Execution/Workflow/WorkflowExpansionService.h"

namespace {
using savor::db::execution::workflow::NormalizeExactFirstBattleExpansionTargets;
using savor::db::execution::workflow::WorkflowExpansionTarget;

TEST(WorkflowExpansionTargets, PreservesExactRequestedCoordinates) {
    const auto targets = NormalizeExactFirstBattleExpansionTargets({
        {.neutral_epoch_count=5, .rtc_value=355},
        {.neutral_epoch_count=0, .rtc_value=355},
        {.neutral_epoch_count=1, .rtc_value=356},
    });
    const std::vector<WorkflowExpansionTarget> expected{
        {0,355}, {1,356}, {5,355},
    };
    EXPECT_EQ(targets, expected);
}

TEST(WorkflowExpansionTargets, DeduplicatesAndRejectsInvalidCoordinates) {
    const auto targets = NormalizeExactFirstBattleExpansionTargets({
        {.neutral_epoch_count=1, .rtc_value=9},
        {.neutral_epoch_count=1, .rtc_value=9},
        {.neutral_epoch_count=-1, .rtc_value=9},
        {.neutral_epoch_count=2, .rtc_value=-1},
    });
    const std::vector<WorkflowExpansionTarget> expected{{1,9}};
    EXPECT_EQ(targets, expected);
}
}
