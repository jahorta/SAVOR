#define NOMINMAX
#include "ArtifactsPane.h"

#include "imgui.h"
#include "imgui_internal.h"
#include <filesystem>
#include <fstream>
#include <thread>
#include <atomic>
#include <mutex>
#include <optional>
#include <vector>

#include "DB/DBCore/ObjectStore.h"
#include "DB/Querying/Paging.h"
#include "DB/Querying/PagedQuery.h"
#include "DB/Querying/IdRepoListDTO.h"
#include "../../Components/ToastBus.h"
#include "../../Components/FutureQueue.h"
#include "../../Components/DropInbox.h"
#include "../../Components/Win32Dialogs.h"
#include "Utils/Hash.h" // sha256(ptr,size)

using namespace simcore::db;
namespace fs = std::filesystem;

namespace {
    struct State {
        // filters
        char search[128]{};
        char ext[32]{};
        int page_limit = 100;

        // paging
        std::optional<KeysetCursor> before{};
        std::optional<KeysetCursor> after{};
        Page<ObjectRefLite> page{};
        bool fetch_in_flight = false;
        std::future<DbResult<Page<ObjectRefLite>>> fut_page;

        // UI
        bool have_roots = false;
        bool import_modal_open = false;
        bool import_modal_request_open = false;
        std::string import_src_path;
        char import_filename[256]{};
        int import_compression = 0; // Compression::None index
        std::atomic<bool> import_busy{ false };
        int selected_row_id{ 0 };

        // table rect (screen-space) for drop hit-test
        ImVec2 table_min{}, table_max{};

        // ephemeral tooltip for multi-file rejection
        bool show_reject_tip = false;
        ImVec2 reject_tip_pos{};
        double reject_tip_until_ms = 0.0;
    };

    static State& S() { static State s; return s; }

    static std::string human_size(int64_t v) {
        const char* units[] = { "B","KB","MB","GB","TB" };
        double d = (double)v;
        int u = 0;
        while (d >= 1024.0 && u < 4) { d /= 1024.0; ++u; }
        char buf[64]; snprintf(buf, sizeof(buf), (u == 0) ? "%.0f %s" : "%.1f %s", d, units[u]);
        return buf;
    }

    static std::string fmt_time(int64_t epoch_sec) {
        std::time_t t = (std::time_t)epoch_sec;
        char buf[32]{ 0 };
#if defined(_WIN32)
        std::tm tm{};
        localtime_s(&tm, &t);
#else
        std::tm tm = *std::localtime(&t);
#endif
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
        return std::string(buf);
    }

    static void normalize_ext(char* ext_buf) {
        if (ext_buf[0] == 0) return;
        if (ext_buf[0] != '.') {
            char tmp[32]{ '.',0 };
            strncat_s(tmp, sizeof(tmp), ext_buf, _TRUNCATE);
            strncpy_s(ext_buf, sizeof(tmp), tmp, _TRUNCATE);
        }
    }

    static void kick_fetch() {
        auto& s = S();
        if (s.fetch_in_flight) return;
        s.fetch_in_flight = true;

        PagedQuery<> q{};
        q.before = s.before;
        q.after = s.after;
        q.limit = s.page_limit;

        std::string search = s.search;
        std::string ext = s.ext;
        s.fut_page = ObjectRefList::ListPagedAsync(q, search, ext);
    }

    static void consume_fetch_if_ready() {
        auto& s = S();
        if (!s.fetch_in_flight) return;
        using namespace std::chrono_literals;
        if (s.fut_page.valid() && s.fut_page.wait_for(0ms) == std::future_status::ready) {
            auto r = s.fut_page.get();
            s.fetch_in_flight = false;
            if (r.ok) {
                s.page = std::move(r.value);
            }
            else {
                GuiToastBus::Error("Artifacts: list failed", r.error.message.c_str());
                s.page.items.clear();
            }
        }
    }

    static bool point_in_rect(const ImVec2& p, const ImVec2& mn, const ImVec2& mx) {
        return p.x >= mn.x && p.y >= mn.y && p.x <= mx.x && p.y <= mx.y;
    }

    static void draw_roots_unset_banner() {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(220, 120, 120, 255));
        ImGui::TextUnformatted("ObjectStore roots are unset. Artifacts view/actions are disabled.");
        ImGui::PopStyleColor();
        ImGui::Separator();
    }

    static void handle_drop_events_when_active() {
        auto& s = S();
        DropEvent ev{};
        while (DropInbox::TryPop(ev)) {
            // only accept if point is inside the table rect
            ImVec2 pt(static_cast<float>(ev.screen_pt.x), static_cast<float>(ev.screen_pt.y));
            if (!point_in_rect(pt, s.table_min, s.table_max)) continue;

            if (ev.paths.size() != 1) {
                s.show_reject_tip = true;
                s.reject_tip_pos = pt;
                s.reject_tip_until_ms = ImGui::GetTime() * 1000.0 + 2000.0;
                continue;
            }

            const fs::path& p = ev.paths[0];
            if (!fs::exists(p)) {
                GuiToastBus::Error("Drop failed", "File does not exist");
                continue;
            }

            s.import_src_path = p.string();
            std::string base = p.filename().string();
            strncpy_s(s.import_filename, base.c_str(), _TRUNCATE);
            s.import_compression = (int)Compression::None;
            s.import_modal_open = true;
            s.import_modal_request_open = true;
        }
    }

    static void maybe_draw_reject_tooltip() {
        auto& s = S();
        if (!s.show_reject_tip) return;
        double now = ImGui::GetTime() * 1000.0;
        if (now > s.reject_tip_until_ms) { s.show_reject_tip = false; return; }
        ImGui::SetNextWindowPos(s.reject_tip_pos, ImGuiCond_Always, ImVec2(0.5f, 1.2f));
        ImGui::BeginTooltip();
        ImGui::TextUnformatted("Drop exactly one file.");
        ImGui::EndTooltip();
    }

    static void draw_import_modal() {
        auto& s = S();
        if (!ImGui::BeginPopupModal("Import Artifact", &s.import_modal_open, ImGuiWindowFlags_AlwaysAutoResize)) return;

        ImGui::Text("Source: %s", s.import_src_path.c_str());
        ImGui::InputText("Filename", s.import_filename, IM_ARRAYSIZE(s.import_filename));
        const char* comp_items[] = { "None" }; // Compression::None only for now
        ImGui::Combo("Compression", &s.import_compression, comp_items, IM_ARRAYSIZE(comp_items));

        if (s.import_busy.load()) {
            ImGui::BeginDisabled();
            ImGui::Button("Importing...");
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Close")) { s.import_modal_open = false; ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
            return;
        }

        bool ok = ImGui::Button("Import");
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) { s.import_modal_open = false; ImGui::CloseCurrentPopup(); }

        if (ok) {
            s.import_busy.store(true);
            std::string src = s.import_src_path;
            std::string filename = s.import_filename;
            Compression comp = Compression::None;

            std::thread([src, filename, comp]() mutable {
                // pre-hash for dedupe notification
                bool existed = false;
                std::string sha = hash::sha256_of_file(src);
                if (!sha.empty()) {
                    auto fut = ObjectStore::GetByShaAsync(sha);
                    auto r = fut.get();
                    existed = r.ok;
                }
                // finalize
                auto fut2 = ObjectStore::FinalizeFromFileAsync(src, comp, filename);
                auto r2 = fut2.get();
                if (r2.ok) {
                    if (existed) GuiToastBus::Info("Import complete (deduped)");
                    else         GuiToastBus::Success("Import complete");
                }
                else {
                    GuiToastBus::Error("Import failed", r2.error.message.c_str());
                }
                auto& s2 = S();
                s2.import_busy.store(false);
                s2.import_modal_open = false;
                // refresh page (newest page)
                s2.before.reset(); s2.after.reset();
                kick_fetch();
                }).detach();

            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    static void do_materialize_to(const ObjectRefLite& r) {
        std::optional<fs::path> outp = Win32Dialogs::SaveFileDialog(L"Materialize to...", fs::path(r.filename).wstring().c_str());
        if (!outp) return;

        FutureQueue::Enqueue(
            ObjectStore::MaterializeToPathAsync(r.id, outp.value().string()),
            // on success
            [](DbResult<void> result)
            {
                if (result.ok) GuiToastBus::Success("Materialized");
                else GuiToastBus::Error("Materialize failed", result.error.message.c_str());
            },
            //on failure
            [](std::exception_ptr e)
            {
                GuiToastBus::Error("Write failed");
                return;
            }
        );
    }
}

void ArtifactsPane::OnActivated() {
    auto& s = S();
    s.page_limit = 100;
    s.search[0] = 0;
    s.ext[0] = 0;
    s.before.reset(); s.after.reset();
    s.have_roots = ObjectStore::Ready();
    if (s.have_roots) kick_fetch();
}

void ArtifactsPane::Draw() {
    auto& s = S();

    ImGui::Begin("Artifacts", nullptr, ImGuiWindowFlags_NoMove);

    s.have_roots = ObjectStore::Ready();
    if (!s.have_roots) {
        draw_roots_unset_banner();
        ImGui::End();
        return;
    }

    ImGui::TextUnformatted("Filter:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200);
    ImGui::InputTextWithHint("##search", "filename contains...", s.search, IM_ARRAYSIZE(s.search));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::InputTextWithHint("##ext", ".sav/.dtm/.bctx", s.ext, IM_ARRAYSIZE(s.ext));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    ImGui::SliderInt("Page", &s.page_limit, 50, 500);
    ImGui::SameLine();
    if (ImGui::Button("Apply")) {
        normalize_ext(s.ext);
        s.before.reset(); s.after.reset();
        kick_fetch();
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset")) {
        s.search[0] = 0; s.ext[0] = 0; s.page_limit = 100;
        s.before.reset(); s.after.reset();
        kick_fetch();
    }

    ImGui::Separator();

    bool has_prev = s.page.prev.has_value();
    bool has_next = s.page.next.has_value();
    if (has_prev) { if (ImGui::Button("Newer")) { s.before = s.page.prev; s.after.reset(); kick_fetch(); } }
    else { ImGui::BeginDisabled(); ImGui::Button("Newer"); ImGui::EndDisabled(); }
    ImGui::SameLine();
    if (has_next) { if (ImGui::Button("Older")) { s.after = s.page.next; s.before.reset(); kick_fetch(); } }
    else { ImGui::BeginDisabled(); ImGui::Button("Older"); ImGui::EndDisabled(); }

    ImGui::BeginChild("list", ImVec2(0, 0), false);
    ImVec2 list_min = ImGui::GetWindowPos();
    ImVec2 list_max = ImVec2(list_min.x + ImGui::GetWindowSize().x, list_min.y + ImGui::GetWindowSize().y);

    if (ImGui::BeginTable("objs", 6, ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Filename", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn("Compression", ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn("SHA-256", ImGuiTableColumnFlags_WidthFixed, 180);
        ImGui::TableSetupColumn("Created", ImGuiTableColumnFlags_WidthFixed, 160);
        ImGui::TableHeadersRow();
        
        ImGuiTable* table = ImGui::GetCurrentTable();
        if (table) {
            const ImRect& inner = table->InnerRect;

            s.table_min = inner.Min;
            s.table_max = inner.Max;
        }

        ImGuiListClipper clipper;
        clipper.Begin((int)s.page.items.size());
        while (clipper.Step())
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const auto& r = s.page.items[i];
                ImGui::PushID((int)r.id);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);

                // Remember where the row starts, so we can overlay later.
                ImVec2 row_top = ImGui::GetCursorScreenPos();
                
                ImGui::Text("%lld", (long long)r.id);

                ImGui::TableSetColumnIndex(1); 
                ImGui::TextUnformatted(r.filename.c_str());

                ImGui::TableSetColumnIndex(2); 
                ImGui::TextUnformatted(human_size(r.size).c_str());

                ImGui::TableSetColumnIndex(3); 
                ImGui::Text("%lld", (long long)r.compression);

                ImGui::TableSetColumnIndex(4);
                {
                    std::string short_sha = r.sha256.size() > 12 ? (r.sha256.substr(0, 12) + "...") : r.sha256;
                    ImGui::TextUnformatted(short_sha.c_str());
                    if (ImGui::BeginPopupContextItem("row_ctx")) {
                        if (ImGui::MenuItem("Materialize to…")) { do_materialize_to(r); }
                        if (ImGui::MenuItem("Copy SHA-256")) { ImGui::SetClipboardText(r.sha256.c_str()); GuiToastBus::Info("Copied SHA-256"); }
                        if (ImGui::MenuItem("Copy ID")) { std::string id = std::to_string(r.id); ImGui::SetClipboardText(id.c_str()); GuiToastBus::Info("Copied ID"); }
                        ImGui::EndPopup();
                    }
                }
                ImGui::TableSetColumnIndex(5); ImGui::TextUnformatted(fmt_time(r.created_at).c_str());
                
                ImGui::TableSetColumnIndex(0);
                ImGui::SetCursorScreenPos(row_top);
                const float row_h = ImGui::GetTextLineHeightWithSpacing();

                bool selected = (S().selected_row_id == r.id);
                ImGuiSelectableFlags sflags =
                    ImGuiSelectableFlags_SpanAllColumns |
                    ImGuiSelectableFlags_AllowItemOverlap;

                if (ImGui::Selectable("##RowOverlay", selected, sflags, ImVec2(0, row_h))) {
                    // optional: left-click selection
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                        S().selected_row_id = r.id;
                }

                // Right-click anywhere on the row opens the same popup
                if (ImGui::BeginPopupContextItem("RowCtx")) {
                    if (ImGui::MenuItem("Materialize to…")) {
                        // open Save File dialog, then call ObjectStore::MaterializeToPathAsync(row.id, chosen_path)
                        do_materialize_to(r);
                    }
                    if (ImGui::MenuItem("Copy SHA-256")) {
                        ImGui::SetClipboardText(r.sha256.c_str());
                    }
                    if (ImGui::MenuItem("Copy ID")) {
                        ImGui::SetClipboardText(std::to_string(r.id).c_str());
                    }
                    ImGui::EndPopup();
                }

                ImGui::PopID();
            }

        ImGui::EndTable();
    }
    ImGui::EndChild();

    consume_fetch_if_ready();
    handle_drop_events_when_active();
    maybe_draw_reject_tooltip();

    ImGui::End();
    if (s.import_modal_request_open) {
        ImGui::OpenPopup("Import Artifact");   // Open from the same context as BeginPopupModal
        s.import_modal_request_open = false;
    }
    draw_import_modal();
}
