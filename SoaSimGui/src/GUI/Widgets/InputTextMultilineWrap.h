#pragma once
#include "imgui.h"
#include "imgui_internal.h"
#include "misc/cpp/imgui_stdlib.h"
#include <string>
#include <algorithm>

struct WrapConfig {
    float wrap_px = 0.0f;        // 0 = derive from widget width
    bool  break_long_words = true; // if a single token exceeds the width, break inside it
    int   tab_as_spaces = 4;     // tabs rendered as N spaces (approx)
};

namespace detail_wrap
{
    struct WrapUserData {
        WrapConfig cfg;
        float inner_px = 0.0f;
        bool  in_rewrap = false;
    };

    // Append UTF-8 bytes [p, p+len)
    inline void append_bytes(std::string& out, const char* p, int len) {
        out.append(p, p + len);
    }

    // Decode one UTF-8 codepoint, return byte count (>=1), cp set to Unicode scalar or 0 on error.
    // Uses a minimal decoder (enough for measuring/advance). Safe for ASCII; for invalid sequences returns 1.
    inline int utf8_next(const char* s, int n, unsigned int& cp) {
        if (n <= 0) { cp = 0; return 0; }
        unsigned char c = (unsigned char)s[0];
        if (c < 0x80) { cp = c; return 1; }
        // 2-byte
        if ((c & 0xE0) == 0xC0 && n >= 2) { cp = ((c & 0x1F) << 6) | (s[1] & 0x3F); return 2; }
        // 3-byte
        if ((c & 0xF0) == 0xE0 && n >= 3) { cp = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F); return 3; }
        // 4-byte
        if ((c & 0xF8) == 0xF0 && n >= 4) { cp = ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F); return 4; }
        cp = c; // fall back
        return 1;
    }

    // Measure advance of a single codepoint with current font.
    inline float advance_for_cp(ImFont* font, unsigned int cp, float font_size) {
        if (cp == '\t') {
            float sp = font->GetCharAdvance(' ');
            return sp; // will be multiplied by tab_as_spaces by caller
        }
        if (cp == '\r') return 0.0f;
        if (cp == '\n') return 0.0f;
        const ImWchar w = (ImWchar)cp;
        const ImFontGlyph* g = font->FindGlyphNoFallback(w);
        if (g) return g->AdvanceX * (font_size / font->FontSize);
        // Fallback advance
        return font->GetCharAdvance('?') * (font_size / font->FontSize);
    }

    // Hard-wrap buffer into 'out' given max line width. Also maps cursor position.
    // cursor_in/out are byte indices into input/output.
    inline void rewrap_to_width(const char* buf, int len, float max_px, const WrapConfig& cfg, int cursor_in, std::string& out, int& cursor_out)
    {
        ImFont* font = ImGui::GetFont();
        const float font_size = ImGui::GetFontSize();
        out.clear();
        out.reserve(len + len / 32 + 16); // small growth hint
        cursor_out = 0;

        float line_px = 0.0f;
        int   out_line_start = 0;

        // Track last breakable spot on this line (space/hyphen/tab)
        int   last_break_out = -1;
        float last_break_line_px = 0.0f;

        for (int i = 0; i < len; ) {
            unsigned int cp = 0;
            int cbytes = utf8_next(buf + i, len - i, cp);
            if (cbytes <= 0) break;

            // Newline resets state
            if (cp == '\n') {
                if (i < cursor_in) cursor_out = (int)out.size();
                out.push_back('\n');
                line_px = 0.0f;
                out_line_start = (int)out.size();
                last_break_out = -1;
                last_break_line_px = 0.0f;
                i += cbytes;
                continue;
            }
            if (cp == '\r') { // skip
                i += cbytes;
                continue;
            }

            float adv = advance_for_cp(font, cp, font_size);
            if (cp == '\t') adv *= (float)std::max(1, cfg.tab_as_spaces);

            const bool is_breakable = (cp == ' ' || cp == '\t' || cp == '-');

            // If this single token would overflow and we're allowed to break long words,
            // we may chop inside the token by forcing a line break before it when needed.
            auto commit_break_at = [&](int where_out_index, bool after_hyphen) {
                // Replace space at break point with newline, or insert newline after hyphen.
                if (where_out_index >= 0 && where_out_index < (int)out.size()) {
                    if (out[where_out_index] == '-') {
                        // Keep hyphen; insert '\n' AFTER it
                        out.insert(out.begin() + where_out_index + 1, '\n');
                        // If cursor was after that insertion point, bump it
                        if (cursor_out > where_out_index) cursor_out++;
                    }
                    else {
                        out[where_out_index] = '\n';
                    }
                    out_line_start = where_out_index + 1;
                }
                else {
                    out.push_back('\n');
                    out_line_start = (int)out.size();
                }
                // Recompute current line width from new line start to end
                line_px = ImGui::CalcTextSize(out.c_str() + out_line_start, out.c_str() + (int)out.size(), false).x;
                last_break_out = -1;
                last_break_line_px = 0.0f;
                };

            // If adding this glyph would overflow the line
            if (max_px > 0.0f && line_px + adv > max_px) {
                if (last_break_out >= 0) {
                    // Wrap at last break point (space/tab becomes '\n', hyphen keeps hyphen + inserts '\n')
                    commit_break_at(last_break_out, out[last_break_out] == '-');
                }
                else if (cfg.break_long_words) {
                    // No breakable point: force a break before this codepoint
                    if ((int)out.size() > out_line_start) {
                        out.push_back('\n');
                        out_line_start = (int)out.size();
                        line_px = 0.0f;
                    }
                }
            }

            // Remember breakable spot position *after* potential wrap
            if (is_breakable) {
                last_break_out = (int)out.size();
                last_break_line_px = line_px + adv;
            }

            if (i < cursor_in) cursor_out = (int)out.size();

            append_bytes(out, buf + i, cbytes);
            line_px += adv;
            i += cbytes;
        }

        // If cursor was at end of input, map to end of output
        if (cursor_in >= len) cursor_out = (int)out.size();
    }

    static int Callback(ImGuiInputTextCallbackData* data)
    {
        auto* ud = reinterpret_cast<WrapUserData*>(data->UserData);
        if (ud->in_rewrap) return 0;
        if (data->EventFlag != ImGuiInputTextFlags_CallbackEdit) return 0;

        ud->in_rewrap = true;

        const float inner_w = ud->inner_px; // already computed by outer fn
        std::string wrapped;
        int new_cursor = data->CursorPos;

        // Build wrapped text + cursor mapping
        rewrap_to_width(data->Buf, data->BufTextLen, inner_w, ud->cfg, data->CursorPos, wrapped, new_cursor);

        // Only rewrite if changed to avoid caret jitter
        bool changed = (wrapped.size() != (size_t)data->BufTextLen) ||
            (memcmp(wrapped.data(), data->Buf, (size_t)data->BufTextLen) != 0);

        if (changed) {
            data->DeleteChars(0, data->BufTextLen);
            data->InsertChars(0, wrapped.c_str(), wrapped.c_str() + wrapped.size());
            data->CursorPos = (int)ImClamp(new_cursor, 0, data->BufTextLen);
        }

        ud->in_rewrap = false;
        return 0;
    }
} // namespace detail_wrap

// Public API
// Returns true if value changed (same semantics as ImGui::InputText*).
inline bool InputTextMultilineWordWrap(const char* label,
    std::string* text,
    const ImVec2& size = ImVec2(0, 0),
    const WrapConfig& cfg = {})
{
    using namespace detail_wrap;

    ImGuiStyle& style = ImGui::GetStyle();
    float box_w = size.x > 0.0f ? size.x : ImGui::GetContentRegionAvail().x;
    float inner_w = box_w - style.FramePadding.x * 2.0f;
    if (cfg.wrap_px > 0.0f) inner_w = cfg.wrap_px;

    WrapUserData ud{};
    ud.cfg = cfg;
    ud.inner_px = std::max(0.0f, inner_w);

    ImGuiInputTextFlags flags =
        ImGuiInputTextFlags_AllowTabInput |
        ImGuiInputTextFlags_CallbackEdit; // std::string overload adds CallbackResize for us

    return ImGui::InputTextMultiline(label, text, size, flags, detail_wrap::Callback, &ud);
}
