#pragma once
#include <cstdint>
#include <vector>

#include "Core/InputCommon/GCPadStatus.h"
#include "../../Utils/Hex.h"

namespace simcore {

    enum ElementFamily : uint8_t { Neutral = 0, Main, CStick, Triggers };

	// GC-like buttons (you can add more later)
    enum : uint16_t {
        GC_DL = PAD_BUTTON_LEFT,   // 0x0001
        GC_DR = PAD_BUTTON_RIGHT,  // 0x0002
        GC_DD = PAD_BUTTON_DOWN,   // 0x0004
        GC_DU = PAD_BUTTON_UP,     // 0x0008
        GC_Z = PAD_TRIGGER_Z,     // 0x0010
        GC_R_BTN = PAD_TRIGGER_R,     // 0x0020
        GC_L_BTN = PAD_TRIGGER_L,     // 0x0040
        // 0x0080 is intentionally unused in Dolphin's GC mask
        GC_A = PAD_BUTTON_A,      // 0x0100
        GC_B = PAD_BUTTON_B,      // 0x0200
        GC_X = PAD_BUTTON_X,      // 0x0400
        GC_Y = PAD_BUTTON_Y,      // 0x0800
        GC_START = PAD_BUTTON_START,  // 0x1000
    };
	struct GCInputFrame {
		uint16_t buttons = 0;
		uint8_t  main_x = 128, main_y = 128;  // 0..255, 128 = center
		uint8_t  c_x = 128, c_y = 128;  // 0..255
		uint8_t  trig_l = 0, trig_r = 0;    // 0..255

        // Named convenience methods (extend as needed)
        inline GCInputFrame& A() { return btn(GC_A); }
        inline GCInputFrame& B() { return btn(GC_B); }
        inline GCInputFrame& X() { return btn(GC_X); }
        inline GCInputFrame& Y() { return btn(GC_Y); }
        inline GCInputFrame& Z() { return btn(GC_Z); }
        inline GCInputFrame& Start() { return btn(GC_START); }
        inline GCInputFrame& DUp() { return btn(GC_DU); }
        inline GCInputFrame& DDown() { return btn(GC_DD); }
        inline GCInputFrame& DLeft() { return btn(GC_DL); }
        inline GCInputFrame& DRight() { return btn(GC_DR); }
        inline GCInputFrame& L() { return btn(GC_L_BTN); }
        inline GCInputFrame& R() { return btn(GC_R_BTN); }
        inline GCInputFrame& Triggers(uint8_t l, uint8_t r) { return trigger(l, r); }
        inline GCInputFrame& TrigL(uint8_t l) { return trigger(l, trig_r); }
        inline GCInputFrame& TrigR(uint8_t r) { return trigger(trig_l, r); }
        inline GCInputFrame& CStick(uint8_t x, uint8_t y) { return stk_c(x, y); }
        inline GCInputFrame& C_X(uint8_t x) { return stk_c(x, c_y); }
        inline GCInputFrame& C_Y(uint8_t y) { return stk_c(c_x, y); }
        inline GCInputFrame& JStick(uint8_t x, uint8_t y) { return stk_main(x, y); }
        inline GCInputFrame& J_X(uint8_t x) { return stk_c(x, main_y); }
        inline GCInputFrame& J_Y(uint8_t y) { return stk_c(main_x, y); }

        template <typename... Bs>
        static inline GCInputFrame new_btns(Bs... bs) { GCInputFrame f{}; f.buttons = static_cast<uint16_t>((0u | ... | static_cast<uint16_t>(bs))); return f; }
        static inline GCInputFrame new_stk_main(uint8_t x, uint8_t y) { GCInputFrame f{}; f.main_x = x; f.main_y = y; return f; }
        static inline GCInputFrame new_stk_c(uint8_t x, uint8_t y) { GCInputFrame f{}; f.c_x = x; f.c_y = y; return f; }
        static inline GCInputFrame new_trigger(uint8_t l, uint8_t r) { GCInputFrame f{}; f.trig_l = l; f.trig_r = r; return f; }

        inline std::string to_frame_hex() {
            static_assert(std::is_trivially_copyable_v<GCInputFrame>, "GCInputFrame must be trivially copyable");
            static_assert(sizeof(GCInputFrame) == 8, "GCInputFrame must be 8 bytes");
            unsigned char buf[sizeof(GCInputFrame)];
            std::memcpy(buf, this, sizeof(buf));
            return bytes_to_hex(buf, sizeof(buf));
        }

        inline size_t get_family() const {
            if (main_x != 128 || main_y != 128) return (size_t)ElementFamily::Main;
            if (c_x != 128 || c_y != 128) return (size_t)ElementFamily::CStick;
            if (trig_l != 0 || trig_r != 0) return (size_t)ElementFamily::Triggers;
            return (size_t)ElementFamily::Neutral;
        }

        bool operator==(const GCInputFrame& other) const {
            return (
                buttons == other.buttons &&
                main_x == other.main_x &&
                main_y == other.main_y&&
                c_x == other.c_x &&
                c_y == other.c_y&&
                trig_l == other.trig_l &&
                trig_r == other.trig_r
                );
        }

    private:

        inline GCInputFrame& btn(uint16_t b) { buttons = buttons | b; return *this; }
        inline GCInputFrame& stk_main(uint8_t x, uint8_t y) { main_x = x; main_y = y; return *this; }
        inline GCInputFrame& stk_c(uint8_t x, uint8_t y) { c_x = x; c_y = y; return *this; }
        inline GCInputFrame& trigger(uint8_t l, uint8_t r) { trig_l = l; trig_r = r; return *this; }
	};

    // Convert signed stick component (-128..127) to 0..255, centered at 128.
    inline constexpr uint8_t SaturateStickToU8(int v_signed)
    {
        int v = v_signed + 128;          // center 0 -> 128
        if (v < 0)   v = 0;
        if (v > 255) v = 255;
        return static_cast<uint8_t>(v);
    }

    // Convert a Dolphin GCPadStatus to our GCInputFrame (1:1 for buttons/triggers).
    inline GCInputFrame FromGCPadStatus(const GCPadStatus& s)
    {
        GCInputFrame f{};
        // Buttons: GCPadStatus.button uses the standard GC bit layout; just copy.
        f.buttons = static_cast<uint16_t>(s.button);

        // Main stick (already 0..255)
        f.main_x = s.stickX;
        f.main_y = s.stickY;

        // C-stick (already 0..255)
        f.c_x = s.substickX;
        f.c_y = s.substickY;

        // Analog triggers (already 0..255)
        f.trig_l = s.triggerLeft;
        f.trig_r = s.triggerRight;

        // Note: s.analogA / s.analogB exist but we leave digital A/B in f.buttons.
        return f;
    }

    // Convenience overload if you have a pointer
    inline GCInputFrame FromGCPadStatus(const GCPadStatus* s)
    {
        return s ? FromGCPadStatus(*s) : GCInputFrame{};
    }

	using InputPlan = std::vector<GCInputFrame>;

} // namespace simcore
