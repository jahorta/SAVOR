#pragma once
#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"
#include <string>
#include <optional>
#include <cstdint>
#include "Utils/IniDoc.h"
#include "imgui_internal.h"

struct IniEditorModalState {
    bool request_open{ false };
    bool in_flight{ false };
    bool visible{ false };
    std::string buffer;
    bool parsed_ok{ true };
    int section_count{ 0 };
    int pair_count{ 0 };
    bool ok_clicked{ false };
    bool cancel_clicked{ false };
    bool dismiss_after_success{ false };
    std::optional<std::string> error_msg{};
};

namespace Widgets {

    inline void OpenIniEditor(IniEditorModalState& s, std::string initial_text) {
        s.buffer = std::move(initial_text);
        s.request_open = true;
        s.in_flight = false;
        s.parsed_ok = true;
        s.section_count = 0;
        s.pair_count = 0;
        s.ok_clicked = false;
        s.cancel_clicked = false;
        s.dismiss_after_success = false;
        s.error_msg.reset();
    }

    inline bool DrawIniEditor(IniEditorModalState& s, const char* popup_id, const char* title,
        const ImRect* center_rect = nullptr, bool center_on_appearing = true)
    {
        if (s.request_open) {
            ImGui::OpenPopup(popup_id);
            s.request_open = false;
            s.visible = true;
        }

        if (center_on_appearing) {
            ImGuiIO& io = ImGui::GetIO();
            ImVec2 c = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
            ImGui::SetNextWindowPos(c, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowFocus();
        }

        bool drawing = false;
        if (ImGui::BeginPopupModal(popup_id, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            drawing = true;
            ImGui::TextUnformatted(title);
            ImGui::Separator();

            ImGui::SetNextItemWidth(560.0f);
            ImGui::InputTextMultiline("##ini_buf", &s.buffer, ImVec2(560, 300),
                ImGuiInputTextFlags_AllowTabInput);

            {
                IniDoc doc = IniDoc::parse(s.buffer);
                auto secs = doc.list_sections();
                s.section_count = (int)secs.size();
                int pairs = 0;
                for (const auto& sec : secs) pairs += (int)doc.section_kv(sec).kv.size();
                s.pair_count = pairs;
                s.parsed_ok = true;
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Text("Parsed: %d section(s), %d key(s)", s.section_count, s.pair_count);
            if (!s.buffer.empty() && s.pair_count == 0) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1, 0.6f, 0, 1), "No key=value pairs parsed");
            }
            if (s.error_msg.has_value()) {
                ImGui::TextColored(ImVec4(1, 0, 0, 1), "Error: %s", s.error_msg->c_str());
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::BeginDisabled(s.in_flight);
            if (ImGui::Button("OK")) {
                s.ok_clicked = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                s.cancel_clicked = true;
                ImGui::CloseCurrentPopup();
                s.visible = false;
            }
            ImGui::EndDisabled();

            if (s.dismiss_after_success && !s.in_flight) {
                s.dismiss_after_success = false;
                ImGui::CloseCurrentPopup();
                s.visible = false;
            }

            ImGui::EndPopup();
        }
        else {
            s.visible = false;
        }
        return drawing;
    }

} // namespace Widgets
