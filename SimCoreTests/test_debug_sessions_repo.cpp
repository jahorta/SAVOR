#include "gtest/gtest.h"
#include "DB/Scheduling/DebugSessionsRepo.h"

TEST(DebugSessionsRepo, ActiveStateClassifierMatchesPolicy) {
    using simcore::db::DebugSessionsRepo;

    EXPECT_TRUE(DebugSessionsRepo::IsActiveState("starting"));
    EXPECT_TRUE(DebugSessionsRepo::IsActiveState("launching_worker"));
    EXPECT_TRUE(DebugSessionsRepo::IsActiveState("attach_ready"));
    EXPECT_TRUE(DebugSessionsRepo::IsActiveState("active"));
    EXPECT_TRUE(DebugSessionsRepo::IsActiveState("stopping"));

    EXPECT_FALSE(DebugSessionsRepo::IsActiveState("stopped"));
    EXPECT_FALSE(DebugSessionsRepo::IsActiveState("failed"));
    EXPECT_FALSE(DebugSessionsRepo::IsActiveState(""));
}
