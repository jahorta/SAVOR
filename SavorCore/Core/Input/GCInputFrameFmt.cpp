#include "GCInputFrameFmt.h"

#include <sstream>

namespace savor {
namespace {

bool FramesEqual(const GCInputFrame& lhs, const GCInputFrame& rhs)
{
    return lhs.buttons == rhs.buttons
        && lhs.main_x == rhs.main_x && lhs.main_y == rhs.main_y
        && lhs.c_x == rhs.c_x && lhs.c_y == rhs.c_y
        && lhs.trig_l == rhs.trig_l && lhs.trig_r == rhs.trig_r;
}

std::string DecodeButtons(std::uint16_t mask, const ButtonNameMap& names)
{
    if (names.empty()) {
        std::ostringstream out;
        out << "buttons=" << std::showbase << std::hex << mask << std::dec;
        return out.str();
    }

    std::ostringstream out;
    bool first = true;
    for (const auto& [bit, name] : names) {
        if ((mask & bit) == 0) continue;
        if (!first) out << '+';
        out << name;
        first = false;
    }
    if (first)
        out << "buttons=" << std::showbase << std::hex << mask << std::dec;
    return out.str();
}

void AppendChangedAxes(
    std::ostringstream& out,
    const GCInputFrame& frame,
    const GCInputFrame& neutral)
{
    const auto append = [&out](const char* key, int value, int baseline) {
        if (value != baseline) out << ' ' << key << '=' << value;
    };
    append("JX", frame.main_x, neutral.main_x);
    append("JY", frame.main_y, neutral.main_y);
    append("CX", frame.c_x, neutral.c_x);
    append("CY", frame.c_y, neutral.c_y);
    append("LT", frame.trig_l, neutral.trig_l);
    append("RT", frame.trig_r, neutral.trig_r);
}

} // namespace

std::string DescribeFrame(
    const GCInputFrame& frame,
    const ButtonNameMap& names,
    const GCInputFrame& neutral)
{
    if (FramesEqual(frame, neutral)) return "---";
    std::ostringstream out;
    out << DecodeButtons(frame.buttons, names);
    AppendChangedAxes(out, frame, neutral);
    return out.str();
}

std::string DescribeFrameCompact(
    const GCInputFrame& frame,
    const ButtonNameMap& names,
    const GCInputFrame& neutral)
{
    if (FramesEqual(frame, neutral)) return "---";

    std::vector<std::string> parts;
    if (frame.buttons != neutral.buttons)
        parts.emplace_back(DecodeButtons(frame.buttons, names));

    const auto append = [&parts](const char* key, int value, int baseline) {
        if (value == baseline) return;
        std::ostringstream out;
        out << key << '=' << value;
        parts.emplace_back(out.str());
    };
    append("JX", frame.main_x, neutral.main_x);
    append("JY", frame.main_y, neutral.main_y);
    append("CX", frame.c_x, neutral.c_x);
    append("CY", frame.c_y, neutral.c_y);
    append("LT", frame.trig_l, neutral.trig_l);
    append("RT", frame.trig_r, neutral.trig_r);

    std::ostringstream out;
    for (std::size_t index = 0; index < parts.size(); ++index) {
        if (index != 0) out << ", ";
        out << parts[index];
    }
    return out.str();
}

} // namespace savor
