#include "InputPlanFmt.h"
#include <sstream>

namespace simcore {

    static bool frames_equal(const GCInputFrame& a, const GCInputFrame& b) {
        // Compare explicitly to avoid padding/memcmp pitfalls.
        return a.buttons == b.buttons &&
            a.main_x == b.main_x && a.main_y == b.main_y &&
            a.c_x == b.c_x && a.c_y == b.c_y &&
            a.trig_l == b.trig_l && a.trig_r == b.trig_r;
    }

    static bool is_neutral(const GCInputFrame& f, const GCInputFrame& neutral) {
        return frames_equal(f, neutral);
    }

    static std::string decode_buttons(uint16_t mask, const ButtonNameMap& names) {
        if (names.empty()) {
            std::ostringstream os;
            os << "buttons=" << std::showbase << std::hex << mask << std::dec;
            return os.str();
        }
        std::ostringstream os;
        bool first = true;
        for (const auto& [bit, name] : names) {
            if ((mask & bit) != 0) {
                if (!first) os << "+";
                os << name;
                first = false;
            }
        }
        if (first) { // none matched -> still show value
            os << "buttons=" << std::showbase << std::hex << mask << std::dec;
        }
        return os.str();
    }

    static void append_changed_axes(std::ostringstream& os,
        const GCInputFrame& f,
        const GCInputFrame& neutral) {
        auto append = [&](const char* key, int cur, int base) {
            if (cur != base) {
                os << " " << key << "=" << cur;
            }
            };
        append("JX", static_cast<int>(f.main_x), static_cast<int>(neutral.main_x));
        append("JY", static_cast<int>(f.main_y), static_cast<int>(neutral.main_y));
        append("CX", static_cast<int>(f.c_x), static_cast<int>(neutral.c_x));
        append("CY", static_cast<int>(f.c_y), static_cast<int>(neutral.c_y));
        append("LT", static_cast<int>(f.trig_l), static_cast<int>(neutral.trig_l));
        append("RT", static_cast<int>(f.trig_r), static_cast<int>(neutral.trig_r));
    }

    std::string DescribeChosenInputs(const InputPlan& plan, const std::string sep,
        const GCInputFrame& neutral,
        const ButtonNameMap& btn_names)
    {
        if (plan.empty())
            return "InputPlan: (empty)";

        struct Segment {
            uint32_t start = 0, end = 0; // inclusive
            GCInputFrame frame{};
        };

        std::vector<Segment> segs;
        segs.reserve(plan.size() / 4 + 1);

        Segment cur{};
        cur.start = cur.end = 0;
        cur.frame = plan[0];

        for (uint32_t i = 1; i < static_cast<uint32_t>(plan.size()); ++i) {
            if (frames_equal(plan[i], cur.frame)) {
                cur.end = i;
            }
            else {
                segs.push_back(cur);
                cur.start = cur.end = i;
                cur.frame = plan[i];
            }
        }
        segs.push_back(cur);

        std::ostringstream out;
        out << "InputPlan (" << plan.size() << " frames)";

        uint32_t shown = 0;
        for (const auto& s : segs) {
            if (is_neutral(s.frame, neutral))
                continue;

            out << "  [" << s.start;
            if (s.end > s.start) out << ".." << s.end;
            out << "] " << decode_buttons(s.frame.buttons, btn_names);
            append_changed_axes(out, s.frame, neutral);
            out << sep;
            ++shown;
        }

        if (shown == 0)
            out << "  (no changes vs neutral)";

        return out.str();
    }

    std::string SummarizeChosenInputs(const InputPlan& plan,
        const GCInputFrame& neutral,
        const ButtonNameMap& btn_names,
        size_t max_segments)
    {
        if (plan.empty())
            return "Chosen inputs: (empty)";

        // Build segments same as DescribeChosenInputs, but emit fewer.
        struct Segment { uint32_t s, e; GCInputFrame f; };
        std::vector<Segment> segs;
        segs.reserve(plan.size() / 4 + 1);

        Segment cur{ 0,0, plan[0] };
        for (uint32_t i = 1; i < static_cast<uint32_t>(plan.size()); ++i) {
            if (frames_equal(plan[i], cur.f)) cur.e = i;
            else { segs.push_back(cur); cur = { i,i,plan[i] }; }
        }
        segs.push_back(cur);

        std::ostringstream out;
        out << "Chosen inputs: ";

        size_t emitted = 0;
        bool first = true;
        for (const auto& s : segs) {
            if (is_neutral(s.f, neutral)) continue;
            if (emitted == max_segments) break;
            if (!first) out << " | ";
            first = false;

            out << "f" << s.s;
            if (s.e > s.s) out << ".." << s.e;
            out << ":" << decode_buttons(s.f.buttons, btn_names);

            std::ostringstream diff;
            append_changed_axes(diff, s.f, neutral);
            std::string d = diff.str();
            if (!d.empty()) out << d;

            ++emitted;
        }

        if (emitted == 0) out << "(none)";
        return out.str();
    }

    std::string DescribeFrame(const GCInputFrame& f,
        const ButtonNameMap& names,
        const GCInputFrame& neutral)
    {
        // Keep the same "neutral" convention used elsewhere
        if (frames_equal(f, neutral))
            return "(no changes vs neutral)";

        std::ostringstream out;

        // Buttons (use same name resolution as DescribeChosenInputs segments)
        out << decode_buttons(f.buttons, names);

        // Axes/trigger deltas in the same JX/JY/CX/CY/LT/RT style
        std::ostringstream diff;
        append_changed_axes(diff, f, neutral);
        const std::string d = diff.str();
        if (!d.empty()) out << d;

        return out.str();
    }

    std::string DescribeFrameCompact(const GCInputFrame& f,
        const ButtonNameMap& names,
        const GCInputFrame& neutral)
    {
        // Emit only fields that differ from neutral
        if (frames_equal(f, neutral))
            return "(no changes vs neutral)";

        std::vector<std::string> parts;

        // Buttons: only include if the mask differs from neutral
        if (f.buttons != neutral.buttons)
            parts.emplace_back(decode_buttons(f.buttons, names));

        // Axes and triggers (only changed ones)
        auto append = [&](const char* key, int cur, int base) {
            if (cur != base) {
                std::ostringstream os;
                os << key << "=" << cur;
                parts.emplace_back(os.str());
            }
            };
        append("JX", static_cast<int>(f.main_x), static_cast<int>(neutral.main_x));
        append("JY", static_cast<int>(f.main_y), static_cast<int>(neutral.main_y));
        append("CX", static_cast<int>(f.c_x), static_cast<int>(neutral.c_x));
        append("CY", static_cast<int>(f.c_y), static_cast<int>(neutral.c_y));
        append("LT", static_cast<int>(f.trig_l), static_cast<int>(neutral.trig_l));
        append("RT", static_cast<int>(f.trig_r), static_cast<int>(neutral.trig_r));

        if (parts.empty())
            return "(neutral)";

        std::ostringstream out;
        for (size_t i = 0; i < parts.size(); ++i) {
            if (i) out << ", ";
            out << parts[i];
        }
        return out.str();
    }

} // namespace simcore