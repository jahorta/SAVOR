// tests/TASPadTests.cpp
#include "gtest/gtest.h"
#include "Core/Input/GCPadOverride.h"
#include "Core/Input/InputPlan.h"

using savor::GCPadOverride;
using savor::GCInputFrame;

TEST(GCPadOverride, NeutralCentered)
{
    auto f = GCPadOverride::NeutralFrame();
    EXPECT_EQ(f.buttons, 0u);
    EXPECT_EQ(f.main_x, 128);
    EXPECT_EQ(f.main_y, 128);
    EXPECT_EQ(f.c_x, 128);
    EXPECT_EQ(f.c_y, 128);
    EXPECT_EQ(f.trig_l, 0);
    EXPECT_EQ(f.trig_r, 0);
}

TEST(GCPadOverride, UpdateFrameNoCrash)
{
    GCPadOverride pad(0);
    pad.setFrame(GCPadOverride::NeutralFrame());
    SUCCEED(); // Behavior is exercised when Dolphin polls via the override.
}
