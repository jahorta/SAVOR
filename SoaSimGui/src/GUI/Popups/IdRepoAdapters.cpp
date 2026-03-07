#include "IdRepoAdapters.h"
#include "DB/Querying/DataService.h"
#include "DB/Querying/Paging.h"
#include "DB/Querying/PagedQuery.h"
#include "DB/Querying/SnapshotMailbox.h"
#include "DB/Querying/IdRepoListDTO.h"
#include "../../Components/ToastBus.h"
#include "imgui.h"

#include <unordered_map>

using simcore::db::DbResult;
using simcore::db::DbErrorKind;
using simcore::db::RetryPolicy;

namespace soasim::ui::adapters {

    template <class Row>
    static auto make_submit(
        std::shared_ptr<SnapshotMailbox<Page<Row>>> mbox,
        std::function<std::future<DbResult<Page<Row>>>(const PagedQuery<>&, const std::string&)> fn
    ) {
        return [mbox, fn](const PagedQuery<>& q, std::string_view s) {
            auto fut = fn(q, std::string(s));
            std::thread([mbox, f = std::move(fut)]() mutable {
                auto res = f.get();
                if (!res.ok) {
                    GuiToastBus::Error("Fetch failed");
                    return;
                }
                mbox->push(std::move(res.value));
                }).detach();
            };
    }

    LedgerAdapter<simcore::db::SavestateLite> MakeSavestateAdapter(int page_size) {
        auto mbox = std::make_shared<SnapshotMailbox<Page<simcore::db::SavestateLite>>>();
        LedgerAdapter<simcore::db::SavestateLite> a{};
        a.kind = LedgerKind::Savestate;
        a.title = "Pick Savestate";
        a.page_size = page_size;
        a.columns = {
            {"ID", 80.f, [](auto& r) { return std::to_string(r.id); }},
            {"Type", 80.f, [](auto& r) { return std::to_string(r.savestate_type); }},
            {"Note", 0.f, [](auto& r) { return r.note; }},
            {"ObjRef", 100.f, [](auto& r) { return r.object_ref_id ? std::to_string(*r.object_ref_id) : std::string(); }},
            {"Complete", 80.f, [](auto& r) { return r.complete ? "1" : "0"; }},
        };
        a.get_id = [](auto& r) { return r.id; };
        auto filenames = std::make_shared<std::unordered_map<int64_t, std::string>>();
        a.draw_preview = [filenames](auto& r)
            {
                if (filenames->find(r.id) == filenames->end()) {
                    if (!r.object_ref_id.has_value())
                        filenames->emplace(r.id, "");
                    else {
                        auto so = simcore::db::ObjectStore::Get(r.object_ref_id.value());
                        if (!so.ok)
                        {
                            filenames->emplace(r.id, "");
                            GuiToastBus::Error("Unable to get Object Row for Savestate id=" + std::to_string(r.id));
                        }
                        else {
                            filenames->emplace(r.id, so.value.filename);
                        }
                    }
                }
                const std::string filename = filenames->at(r.id);
                if (!filename.empty()) {
                    ImGui::Text("Filename: %s", filename.c_str());
                }
                else {
                    ImGui::Text("Unable to get Object Row for Savestate id=%lld", (long long)r.id);
                }
            };
        a.poll_snapshot = [mbox]() { return mbox->latest(); };
        a.submit_request = make_submit<simcore::db::SavestateLite>(mbox, [](const PagedQuery<>& q, const std::string& s) {
            return simcore::db::DataService::FetchSavestatesPage(q, s);
            });
        return a;
    }

    LedgerAdapter<simcore::db::ObjectRefLite> MakeObjectRefAdapter(std::string ext_filter, int page_size) {
        auto mbox = std::make_shared<SnapshotMailbox<Page<simcore::db::ObjectRefLite>>>();
        LedgerAdapter<simcore::db::ObjectRefLite> a{};
        a.kind = LedgerKind::ObjectRef;
        a.title = "Pick Artifact";
        a.page_size = page_size;
        a.columns = {
            {"ID", 80.f, [](auto& r) { return std::to_string(r.id); }},
            {"Filename", 0.f, [](auto& r) { return r.filename; }},
            {"Size", 100.f, [](auto& r) { return std::to_string(r.size); }},
        };
        a.get_id = [](auto& r) { return r.id; };
        a.draw_preview = [](auto&) {};
        a.poll_snapshot = [mbox]() { return mbox->latest(); };
        a.submit_request = [mbox, ext = std::move(ext_filter)](const PagedQuery<>& q, std::string_view s) {
            auto fut = simcore::db::DataService::FetchObjectRefsPage(q, std::string(s), ext);
            std::thread([mbox, f = std::move(fut)]() mutable {
                auto res = f.get();
                if (!res.ok) { GuiToastBus::Error("Fetch failed"); return; }
                mbox->push(std::move(res.value));
                }).detach();
            };
        return a;
    }

    LedgerAdapter<simcore::db::SeedProbeLite> MakeSeedProbeAdapter(int page_size, std::optional<int64_t> filter_savestate_id) {
        auto mbox = std::make_shared<SnapshotMailbox<Page<simcore::db::SeedProbeLite>>>();
        LedgerAdapter<simcore::db::SeedProbeLite> a{};
        a.kind = LedgerKind::SeedProbe;
        a.title = "Pick SeedProbe";
        a.page_size = page_size;
        a.columns = {
            {"ID", 80.f, [](auto& r) { return std::to_string(r.id); }},
            {"Savestate", 100.f, [](auto& r) { return std::to_string(r.savestate_id); }},
            {"Neutral", 120.f, [](auto& r) { return r.neutral_seed ? std::to_string(*r.neutral_seed) : std::string(); }},
            {"Status", 100.f, [](auto& r) { return r.status; }},
            {"Complete", 80.f, [](auto& r) { return r.complete ? "1" : "0"; }},
        };
        a.get_id = [](auto& r) { return r.id; };
        a.draw_preview = [](auto&) {};
        a.poll_snapshot = [mbox]() { return mbox->latest(); };
        a.submit_request = [mbox, filter_savestate_id](const PagedQuery<>& q, std::string_view s) {
            auto fut = simcore::db::DataService::FetchSeedProbesPage(q, std::string(s), /*only_done=*/true, filter_savestate_id);
            std::thread([mbox, f = std::move(fut)]() mutable {
                auto res = f.get();
                if (!res.ok) { GuiToastBus::Error("Fetch failed"); return; }
                mbox->push(std::move(res.value));
                }).detach();
            };
        a.open_aux_filter = []() { /* hook up nested savestate picker in PhaseBuilder when wiring */ };
        return a;
    }

    LedgerAdapter<simcore::db::TasMovieLite> MakeTasMovieAdapter(int page_size) {
        auto mbox = std::make_shared<SnapshotMailbox<Page<simcore::db::TasMovieLite>>>();
        LedgerAdapter<simcore::db::TasMovieLite> a{};
        a.kind = LedgerKind::TasMovie;
        a.title = "Pick TasMovie";
        a.page_size = page_size;
        a.columns = {
            {"ID", 80.f, [](auto& r) { return std::to_string(r.id); }},
            {"Base", 100.f, [](auto& r) { return std::to_string(r.base_file_id); }},
            {"NewRTC", 100.f, [](auto& r) { return r.new_rtc ? std::to_string(*r.new_rtc) : std::string(); }},
            {"Status", 100.f, [](auto& r) { return r.status; }},
            {"Created", 120.f, [](auto& r) { return r.created_at ? std::to_string(*r.created_at) : std::string(); }},
        };
        a.get_id = [](auto& r) { return r.id; };
        a.draw_preview = [](auto&) {};
        a.poll_snapshot = [mbox]() { return mbox->latest(); };
        a.submit_request = make_submit<simcore::db::TasMovieLite>(mbox, [](const PagedQuery<>& q, const std::string& s) {
            return simcore::db::DataService::FetchTasMoviesPage(q, s, /*only_done=*/true);
            });
        return a;
    }

    LedgerAdapter<simcore::db::ExplorerSettingsLite> MakeExplorerSettingsAdapter(int page_size)
    {
        auto mbox = std::make_shared<SnapshotMailbox<Page<simcore::db::ExplorerSettingsLite>>>();
        LedgerAdapter<simcore::db::ExplorerSettingsLite> a{};
        a.kind = LedgerKind::BattleRunGroup;
        a.title = "Pick BattleRun Group";
        a.page_size = page_size;
        a.columns = {
            {"ID", 80.f, [](auto& r) { return std::to_string(r.id); }},
            {"Name", 200.f, [](auto& r) { return r.name; }},
            {"Desc", 0.f, [](auto& r) { return r.description; }}
        };
        a.get_id = [](auto& r) { return r.id; };
        a.draw_preview = [](auto&) {};
        a.poll_snapshot = [mbox]() { return mbox->latest(); };
        a.submit_request = make_submit<simcore::db::ExplorerSettingsLite>(mbox, [](const PagedQuery<>& q, const std::string& s) {
            return simcore::db::DataService::FetchExplorerSettingsPage(q, s);
            });
        return a;
    }

} // namespace soasim::ui::adapters
