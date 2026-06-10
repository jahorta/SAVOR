#include "gtest/gtest.h"
#include "Core/Input/InputPlan.h"           // GCInputFrame (with fluent methods)
#include "Core/InputCommon/GCPadStatus.h"   // PAD_BUTTON_* / PAD_TRIGGER_*

static bool bit_set(uint16_t v, uint16_t mask) { return (v & mask) == mask; }

using namespace savor;

TEST(GCInputFrameFluent, NeutralDefaults)
{
    GCInputFrame f{};
    EXPECT_EQ(f.buttons, 0u);
    EXPECT_EQ(f.main_x, 128);
    EXPECT_EQ(f.main_y, 128);
    EXPECT_EQ(f.c_x, 128);
    EXPECT_EQ(f.c_y, 128);
    EXPECT_EQ(f.trig_l, 0);
    EXPECT_EQ(f.trig_r, 0);
}

TEST(GCInputFrameFluent, ChainTwoButtons)
{
    GCInputFrame f = GCInputFrame{}.A().Start();
    const uint16_t expected = PAD_BUTTON_A | PAD_BUTTON_START;

    EXPECT_TRUE(bit_set(f.buttons, PAD_BUTTON_A));
    EXPECT_TRUE(bit_set(f.buttons, PAD_BUTTON_START));
    EXPECT_EQ(f.buttons, expected);               // no extra bits set
}

TEST(GCInputFrameFluent, IdempotentWhenChained)
{
    GCInputFrame f = GCInputFrame{}.A().A().Start().A();
    EXPECT_EQ(f.buttons, (PAD_BUTTON_A | PAD_BUTTON_START));
}

TEST(GCInputFrameFluent, NamedDpadAndTriggers)
{
    GCInputFrame f = GCInputFrame{}.DRight().R().L();

    EXPECT_TRUE(bit_set(f.buttons, PAD_BUTTON_RIGHT));
    EXPECT_TRUE(bit_set(f.buttons, PAD_TRIGGER_R));
    EXPECT_TRUE(bit_set(f.buttons, PAD_TRIGGER_L));

    const uint16_t expected = PAD_BUTTON_RIGHT | PAD_TRIGGER_R | PAD_TRIGGER_L;
    EXPECT_EQ(f.buttons, expected);
}

TEST(GCInputFrameFluent, StaticWithBuilder)
{
    GCInputFrame f = GCInputFrame::new_btns(GC_A, GC_R_BTN);
    EXPECT_EQ(f.buttons, (PAD_BUTTON_A | PAD_TRIGGER_R));
}

TEST(GCInputFrameFluent, ChainOnTemporaryCopiesResult)
{
    // Ensures chaining on a temporary yields a proper value copy.
    GCInputFrame f = GCInputFrame{}.Y().DUp().Z();
    EXPECT_EQ(f.buttons, (PAD_BUTTON_Y | PAD_BUTTON_UP | PAD_TRIGGER_Z));
}
