#include "IdPicker.h"
#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"
#include "DB/Querying/IdRepoListDTO.h"

using soasim::ui::PickResult;

namespace soasim::ui {

    template <class Row>
    static int64_t get_first_id(const Page<Row>& p, const std::function<int64_t(const Row&)>& f) {
        return p.items.empty() ? 0 : f(p.items.front());
    }
    template <class Row>
    static int64_t get_last_id(const Page<Row>& p, const std::function<int64_t(const Row&)>& f) {
        return p.items.empty() ? 0 : f(p.items.back());
    }

    template <class Row>
    bool LedgerPicker<Row>::Draw(std::function<void(const PickResult&, const std::optional<Row>&)> cb) {
        if (!open) return false;
        // Center on first appear relative to main viewport (only as a starting point).
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(720, 150), ImVec2(vp->WorkSize.x, vp->WorkSize.y));

        if (ImGui::BeginPopupModal(args.modal_id, &open, ImGuiWindowFlags_NoResize)) {
            if (search_buffer.empty() && !args.initial_search.empty()) search_buffer = args.initial_search;
            if (refresh) { page.reset(); refresh = false; }
            if (!page.has_value()) { adapter.submit_request(args.initial_query, search_buffer); }

            bool do_search = false;
            ImGui::SetNextItemWidth(-200);
            if (ImGui::InputText("##search", &search_buffer)) do_search = true;
            ImGui::SameLine();
            if (adapter.open_aux_filter) { if (ImGui::Button("Filter…")) adapter.open_aux_filter(); }
			ImGui::SameLine();
            if (ImGui::Button("Refresh")) { refresh = true; }

            auto snap = adapter.poll_snapshot();
            if (snap) page = std::move(*snap);

            ImGui::Separator();
            ImGui::BeginChild("list", ImVec2(ImGui::GetContentRegionAvail().x * 0.66f, ImGui::GetContentRegionAvail().y - 40), false, ImGuiWindowFlags_None);
            if (page && !page->items.empty()) {
                if (ImGui::BeginTable("ledger", (int)adapter.columns.size(), ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
                    for (auto& col : adapter.columns) {
                        if (col.width_px > 0) ImGui::TableSetupColumn(col.label, ImGuiTableColumnFlags_WidthFixed, col.width_px);
                        else ImGui::TableSetupColumn(col.label);
                    }
                    ImGui::TableHeadersRow();
                    int row_idx = 0;
                    for (const Row& r : page->items) {
                        ImGui::PushID(row_idx++);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImVec2 row_top = ImGui::GetCursorScreenPos();
                        const bool is_selected = (selected_id == adapter.get_id(r));
                        for (int c = 0; c < (int)adapter.columns.size(); ++c) {
                            ImGui::TableSetColumnIndex(c);
                            const std::string s = adapter.columns[c].format(r);
                            ImGui::TextUnformatted(s.c_str());
                        }
                        
                        ImGui::SetCursorScreenPos(row_top);
                        const float row_h = ImGui::GetTextLineHeightWithSpacing();
                        if (ImGui::Selectable("##row", is_selected,
                            ImGuiSelectableFlags_SpanAllColumns |
                            ImGuiSelectableFlags_AllowDoubleClick |
                            ImGuiSelectableFlags_AllowItemOverlap,
                            ImVec2(0, row_h)))
                        {
                            selected_id = adapter.get_id(r);
                            current_row = r;
                            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                                PickResult pr{ true, adapter.get_id(r) };
                                ImGui::CloseCurrentPopup();
                                open = false;
                                if (cb) cb(pr, r);
                                ImGui::PopID(); ImGui::EndTable(); ImGui::EndChild(); ImGui::EndPopup();
                                return true;
                            }
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
            }
            else {
                ImGui::TextUnformatted("No rows.");
            }
            ImGui::EndChild();

            ImGui::SameLine();
            ImGui::BeginChild("preview", ImVec2(0, ImGui::GetContentRegionAvail().y - 40), true);
            if (current_row && adapter.draw_preview) adapter.draw_preview(*current_row);
            ImGui::EndChild();

            ImGui::Separator();
            float footer_h = 0;
            ImGui::BeginChild("footer", ImVec2(0, 40), false);
            bool can_prev = page && page->prev.has_value();
            bool can_next = page && page->next.has_value();
            if (ImGui::Button("Prev")) {
                if (page && page->prev) {
                    PagedQuery<> q; q.limit = adapter.page_size; q.before = page->prev; q.order = PageOrder::Desc;
                    adapter.submit_request(q, search_buffer);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Next")) {
                if (page && page->next) {
                    PagedQuery<> q; q.limit = adapter.page_size; q.after = page->next; q.order = PageOrder::Desc;
                    adapter.submit_request(q, search_buffer);
                }
            }
            ImGui::SameLine();
            ImGui::Dummy(ImVec2(20, 0));
            ImGui::SameLine();
            bool ok_enabled = selected_id > 0;
            if (!ok_enabled) ImGui::BeginDisabled();
            if (ImGui::Button("OK")) {
                PickResult pr{ true, selected_id };
                ImGui::CloseCurrentPopup(); open = false; if (cb) cb(pr, current_row);
            }
            if (!ok_enabled) ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                PickResult pr{ false, 0 };
                ImGui::CloseCurrentPopup(); open = false; if (cb) cb(pr, std::nullopt);
            }
            ImGui::EndChild();

            if (do_search) {
                PagedQuery<> q; q.limit = adapter.page_size; q.order = PageOrder::Desc;
                adapter.submit_request(q, search_buffer);
            }
            ImGui::EndPopup();
        }
        return !open;
    }

    // explicit template
    template struct LedgerPicker<simcore::db::SavestateLite>;
    template struct LedgerPicker<simcore::db::SeedProbeLite>;
    template struct LedgerPicker<simcore::db::TasMovieLite>;
    template struct LedgerPicker<simcore::db::ObjectRefLite>;
    template struct LedgerPicker<simcore::db::ExplorerSettingsLite>;

} // namespace soasim::ui
