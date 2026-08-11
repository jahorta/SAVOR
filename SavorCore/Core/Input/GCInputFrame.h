#pragma once

#include "Core/InputCommon/GCPadStatus.h"
#include "../../Utils/Hex.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>

namespace savor {

enum ElementFamily : std::uint8_t
{
    Neutral = 0,
    Main,
    CStick,
    Triggers,
};

enum : std::uint16_t
{
    GC_DL = PAD_BUTTON_LEFT,
    GC_DR = PAD_BUTTON_RIGHT,
    GC_DD = PAD_BUTTON_DOWN,
    GC_DU = PAD_BUTTON_UP,
    GC_Z = PAD_TRIGGER_Z,
    GC_R_BTN = PAD_TRIGGER_R,
    GC_L_BTN = PAD_TRIGGER_L,
    GC_A = PAD_BUTTON_A,
    GC_B = PAD_BUTTON_B,
    GC_X = PAD_BUTTON_X,
    GC_Y = PAD_BUTTON_Y,
    GC_START = PAD_BUTTON_START,
};

struct GCInputFrame
{
    std::uint16_t buttons = 0;
    std::uint8_t main_x = 128;
    std::uint8_t main_y = 128;
    std::uint8_t c_x = 128;
    std::uint8_t c_y = 128;
    std::uint8_t trig_l = 0;
    std::uint8_t trig_r = 0;

    GCInputFrame& A() { return btn(GC_A); }
    GCInputFrame& B() { return btn(GC_B); }
    GCInputFrame& X() { return btn(GC_X); }
    GCInputFrame& Y() { return btn(GC_Y); }
    GCInputFrame& Z() { return btn(GC_Z); }
    GCInputFrame& Start() { return btn(GC_START); }
    GCInputFrame& DUp() { return btn(GC_DU); }
    GCInputFrame& DDown() { return btn(GC_DD); }
    GCInputFrame& DLeft() { return btn(GC_DL); }
    GCInputFrame& DRight() { return btn(GC_DR); }
    GCInputFrame& L() { return btn(GC_L_BTN); }
    GCInputFrame& R() { return btn(GC_R_BTN); }
    GCInputFrame& Triggers(std::uint8_t l, std::uint8_t r)
    {
        return trigger(l, r);
    }
    GCInputFrame& TrigL(std::uint8_t l) { return trigger(l, trig_r); }
    GCInputFrame& TrigR(std::uint8_t r) { return trigger(trig_l, r); }
    GCInputFrame& CStick(std::uint8_t x, std::uint8_t y)
    {
        return stk_c(x, y);
    }
    GCInputFrame& C_X(std::uint8_t x) { return stk_c(x, c_y); }
    GCInputFrame& C_Y(std::uint8_t y) { return stk_c(c_x, y); }
    GCInputFrame& JStick(std::uint8_t x, std::uint8_t y)
    {
        return stk_main(x, y);
    }
    GCInputFrame& J_X(std::uint8_t x) { return stk_main(x, main_y); }
    GCInputFrame& J_Y(std::uint8_t y) { return stk_main(main_x, y); }

    template <typename... Buttons>
    static GCInputFrame new_btns(Buttons... buttons)
    {
        GCInputFrame frame{};
        frame.buttons = static_cast<std::uint16_t>(
            (0u | ... | static_cast<std::uint16_t>(buttons)));
        return frame;
    }
    static GCInputFrame new_stk_main(std::uint8_t x, std::uint8_t y)
    {
        GCInputFrame frame{};
        frame.main_x = x;
        frame.main_y = y;
        return frame;
    }
    static GCInputFrame new_stk_c(std::uint8_t x, std::uint8_t y)
    {
        GCInputFrame frame{};
        frame.c_x = x;
        frame.c_y = y;
        return frame;
    }
    static GCInputFrame new_trigger(std::uint8_t l, std::uint8_t r)
    {
        GCInputFrame frame{};
        frame.trig_l = l;
        frame.trig_r = r;
        return frame;
    }

    std::string to_frame_hex() const
    {
        static_assert(std::is_trivially_copyable_v<GCInputFrame>);
        static_assert(sizeof(GCInputFrame) == 8);
        unsigned char bytes[sizeof(GCInputFrame)];
        std::memcpy(bytes, this, sizeof(bytes));
        return bytes_to_hex(bytes, sizeof(bytes));
    }

    std::size_t get_family() const
    {
        if (main_x != 128 || main_y != 128)
            return static_cast<std::size_t>(ElementFamily::Main);
        if (c_x != 128 || c_y != 128)
            return static_cast<std::size_t>(ElementFamily::CStick);
        if (trig_l != 0 || trig_r != 0)
            return static_cast<std::size_t>(ElementFamily::Triggers);
        return static_cast<std::size_t>(ElementFamily::Neutral);
    }

    bool operator==(const GCInputFrame&) const = default;

private:
    GCInputFrame& btn(std::uint16_t button)
    {
        buttons = static_cast<std::uint16_t>(buttons | button);
        return *this;
    }
    GCInputFrame& stk_main(std::uint8_t x, std::uint8_t y)
    {
        main_x = x;
        main_y = y;
        return *this;
    }
    GCInputFrame& stk_c(std::uint8_t x, std::uint8_t y)
    {
        c_x = x;
        c_y = y;
        return *this;
    }
    GCInputFrame& trigger(std::uint8_t l, std::uint8_t r)
    {
        trig_l = l;
        trig_r = r;
        return *this;
    }
};

inline constexpr std::uint8_t SaturateStickToU8(int value)
{
    value += 128;
    if (value < 0) value = 0;
    if (value > 255) value = 255;
    return static_cast<std::uint8_t>(value);
}

inline GCInputFrame FromGCPadStatus(const GCPadStatus& status)
{
    GCInputFrame frame{};
    frame.buttons = static_cast<std::uint16_t>(status.button);
    frame.main_x = status.stickX;
    frame.main_y = status.stickY;
    frame.c_x = status.substickX;
    frame.c_y = status.substickY;
    frame.trig_l = status.triggerLeft;
    frame.trig_r = status.triggerRight;
    return frame;
}

inline GCInputFrame FromGCPadStatus(const GCPadStatus* status)
{
    return status ? FromGCPadStatus(*status) : GCInputFrame{};
}

} // namespace savor
