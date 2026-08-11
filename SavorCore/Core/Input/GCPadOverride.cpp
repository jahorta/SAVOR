#include "GCPadOverride.h"

// Dolphin includes only in the .cpp to keep the header light
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/CoreDevice.h"
#include "InputCommon/InputConfig.h"
#include "Core/HW/GCPad.h"
#include "Core/InputCommon/GCPadStatus.h"

namespace savor {
    namespace {
        using ControlState = ControlState;

        inline double axis_from_u8(uint8_t v) {
            // [0..255] (128 center) -> [-1..1]
            const double d = (static_cast<int>(v) - 128);
            if (d == 0) return 0.0;
            if (d > 0)  return std::min(1.0, static_cast<double>(d) / 127.0);
            else        return std::max(-1.01, static_cast<double>(d) / 127.0);
    }
        inline double trig_from_u8(uint8_t v) {
            double d = static_cast<double>(v) / 255.0;
            return d < 0.0 ? 0.0 : (d > 1.0 ? 1.0 : d);
        }
    }

    GCInputFrame GCPadOverride::NeutralFrame() {
        GCInputFrame f{};
        f.buttons = 0;
        f.main_x = 128; f.main_y = 128;
        f.c_x = 128; f.c_y = 128;
        f.trig_l = 0;   f.trig_r = 0;
        return f;
    }

    void GCPadOverride::publishFrame(
        uint64_t publication_epoch,
        const GCInputFrame& f) {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_cur = f;
        m_publication_epoch = publication_epoch;
        m_callback_count = 0;
    }

    GCPadOverride::PollStats GCPadOverride::getPollStats() const {
        std::lock_guard<std::mutex> lk(m_mtx);
        return PollStats{
            .publication_epoch = m_publication_epoch,
            .callback_count = m_callback_count,
            .frame = m_cur,
        };
    }

    void GCPadOverride::install() {
        if (m_installed)
            return;

        auto* cfg = Pad::GetConfig();
        if (!cfg) {
            SCLOGI("[TAS] Pad::GetConfig() not ready; will need to call install() after Pad::Initialize()");
            return;
        }
        auto* ctrl = cfg->GetController(m_port);
        if (!ctrl) {
            SCLOGI("[TAS] Controller %d missing; cannot install override", m_port);
            return;
        }

        ControllerEmu::InputOverrideFunction fn =
            [this](std::string_view group, std::string_view control, ControlState def)
            -> std::optional<ControlState>
            {
                GCInputFrame f{};
                {
                    std::lock_guard<std::mutex> lk(m_mtx);
                    f = m_cur;
                    if (m_publication_epoch != 0) {
                        ++m_callback_count;
                    }
                }

                auto is = [&](uint16_t bit) { return (f.buttons & bit) ? 1.0 : 0.0; };
                // Buttons group
                if (group == "Buttons") {
                    if (control == "A")            return is(PAD_BUTTON_A);
                    if (control == "B")            return is(PAD_BUTTON_B);
                    if (control == "X")            return is(PAD_BUTTON_X);
                    if (control == "Y")            return is(PAD_BUTTON_Y);
                    if (control == "Z")            return is(PAD_TRIGGER_Z);
                    if (control == "Start")        return is(PAD_BUTTON_START);
                }

                if (group == "D-Pad") {
                    if (control == "Up")     return is(PAD_BUTTON_UP);
                    if (control == "Down")   return is(PAD_BUTTON_DOWN);
                    if (control == "Left")   return is(PAD_BUTTON_LEFT);
                    if (control == "Right")  return is(PAD_BUTTON_RIGHT);
                }

                // Main stick
                if (group == "Main Stick") {
                    if (control == "X") return axis_from_u8(f.main_x);
                    if (control == "Y") return axis_from_u8(f.main_y);
                }

                // C-Stick
                if (group == "C-Stick") {
                    if (control == "X") return axis_from_u8(f.c_x);
                    if (control == "Y") return axis_from_u8(f.c_y);
                }

                // Triggers (analog + digitals here in some builds)
                if (group == "Triggers") {
                    if (control == "L-Analog") return trig_from_u8(f.trig_l);
                    if (control == "R-Analog") return trig_from_u8(f.trig_r);
                    if (control == "L")        return (f.buttons & PAD_TRIGGER_L) ? 1.0 : 0.0;
                    if (control == "R")        return (f.buttons & PAD_TRIGGER_R) ? 1.0 : 0.0;
                }

                // Not overridden
                return std::nullopt;
            };

        ctrl->SetInputOverrideFunction(std::move(fn));
        {
            std::lock_guard<std::mutex> lock(m_mtx);
            m_cur = NeutralFrame();
            m_publication_epoch = 0;
            m_callback_count = 0;
        }
        m_installed = true;
        SCLOGI("[TAS] Installed input override on GC port %d", m_port);
    }

} // namespace savor
