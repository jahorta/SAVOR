#include "SeedProbePane.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "imgui_internal.h"
#include "misc/cpp/imgui_stdlib.h"

#include <optional>
#include <vector>
#include <unordered_map>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <tuple>
#include <functional>
#include <mutex>

#include "DB/DBCore/DbResult.h"
#include "DB/Querying/PagedQuery.h"
#include "DB/Querying/Paging.h"
#include "DB/Querying/IdRepoListDTO.h"
#include "DB/DBCore/ObjectStore.h"
#include "DB/SavestateRepo.h"
#include "DB/SeedProbeRepo.h"
#include "DB/DeltaSeedRepo.h"
#include "../../Components/FutureQueue.h"
#include "../../Components/ToastBus.h"
#include "../../Components/LazyCache.h"
#include "../../Components/ColorMaps.h"
#include "../Widgets/StatusBar.h"
#include "Core/Input/InputPlan.h"
#include "Core/Input/InputPlanFmt.h"

using namespace simcore::db;

using simcore::GCInputFrame;

namespace {

    struct Bounds { int minx{}, maxx{}, miny{}, maxy{}; };
    struct Step { int sx{ 2 }, sy{ 2 }; };

    struct GridPoint {
        int x{}, y{};
        int delta{};
    };

    struct GridData {
        std::vector<GridPoint> pts;
        Bounds b{};
        Step step{};
        int minNeg{ -2 };
        int maxPos{ 32 };
    };

    struct GraphCacheKey {
        int64_t probe_id{};
        simcore::ElementFamily fam{};
        bool operator==(const GraphCacheKey& o) const { return probe_id == o.probe_id && fam == o.fam; }
    };
    struct GraphCacheKeyHash {
        std::size_t operator()(const GraphCacheKey& k) const {
            return std::hash<int64_t>{}(k.probe_id) ^ (std::hash<int>{}((int)k.fam) << 1);
        }
    };

    template<class T> static int sgn(T v) { return (T(0) < v) - (v < T(0)); }

    static std::string hex32(uint32_t v) {
        std::ostringstream oss; oss << std::hex << std::nouppercase << std::setw(8) << std::setfill('0') << v;
        return oss.str();
    }

    static Step infer_step(const std::vector<GridPoint>& pts) {
        std::vector<int> xs; xs.reserve(pts.size()); for (auto& p : pts) xs.push_back(p.x);
        std::vector<int> ys; ys.reserve(pts.size()); for (auto& p : pts) ys.push_back(p.y);
        auto uniq_sorted = [](std::vector<int>& v) { std::sort(v.begin(), v.end()); v.erase(std::unique(v.begin(), v.end()), v.end()); };
        uniq_sorted(xs); uniq_sorted(ys);
        auto min_delta_gt1 = [](const std::vector<int>& v)->int {
            int s = 0;
            for (int i = 1; i < (int)v.size(); ++i) { int d = v[i] - v[i - 1]; if (d > 1) s = (s == 0 ? d : std::min(s, d)); }
            return s > 1 ? s : 2;
            };
        Step s{}; s.sx = min_delta_gt1(xs); s.sy = min_delta_gt1(ys);
        return s;
    }

    static Bounds infer_bounds(const std::vector<GridPoint>& pts) {
        Bounds b{};
        if (pts.empty()) { b.minx = b.miny = 0; b.maxx = b.maxy = 0; return b; }
        b.minx = b.maxx = pts[0].x;
        b.miny = b.maxy = pts[0].y;
        for (auto& p : pts) {
            b.minx = std::min(b.minx, p.x);
            b.maxx = std::max(b.maxx, p.x);
            b.miny = std::min(b.miny, p.y);
            b.maxy = std::max(b.maxy, p.y);
        }
        return b;
    }

    static void draw_grid_rects(GridData& g, ImVec2 top_left, ImVec2 size)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // 1) Fill entire plot background black (represents the full 0..255 space)
        dl->AddRectFilled(top_left, top_left + size, IM_COL32(0, 0, 0, 255));

        // Nothing to draw?
        if (g.pts.empty() || g.step.sx <= 0 || g.step.sy <= 0) return;

        // 2) Build pixel-snapped edges for the full 0..255 range so there are no seams.
        //    Each cell is drawn using [edge[x], edge[x+step]) so adjacent cells share exact edges.
        const float sx = size.x / 256.0f;
        const float sy = size.y / 256.0f;

        // Rounded pixel edges (257 entries cover 0..256)
        int xedge[257], yedge[257];
        for (int i = 0; i <= 256; ++i) {
            xedge[i] = (int)IM_ROUND(top_left.x + i * sx);
            yedge[i] = (int)IM_ROUND(top_left.y + i * sy);
        }

        // 3) Draw each sampled cell snapped to these edges
        const int step_x = g.step.sx;
        const int step_y = g.step.sy;

        int minNeg = g.minNeg;
        int maxPos = g.maxPos;
        if (maxPos < 1) maxPos = 1;
        if (minNeg > -1) minNeg = -1;

        std::vector<GridPoint> sorted_pts = g.pts;
        std::sort(
            sorted_pts.begin(),
            sorted_pts.end(),
            [](GridPoint a, GridPoint b) 
            {
                if (a.x != b.x) return a.x < b.x;
                return a.y < b.y;
            }
        );

        if (sorted_pts.empty()) return;

        int p_x_max = sorted_pts.front().x;
        int p_y_max = sorted_pts.front().y;
        for (const auto& p : sorted_pts) {
            p_x_max = std::max(p_x_max, p.x);
            p_y_max = std::max(p_y_max, p.y);
        }

        for (const auto& p : sorted_pts) {
            // Clamp to valid input range
            int x0i = std::clamp(p.x, 0, 255);
            int y0i = std::clamp(p.y, 0, 255);

            int x1i; int y1i;
            if (p.x != p_x_max) x1i = std::min(256, x0i + step_x + step_x);
            else x1i = std::min(256, x0i + step_x);
            if (p.y != p_y_max) y1i = std::min(256, y0i + step_y + step_y);
            else y1i = std::min(256, y0i + step_y);


            const ImU32 col = SeedDeltaToColorModB(p.delta, minNeg, maxPos);

            // Convert to pixel rectangle using snapped edges (no gaps)
            ImVec2 tl((float)xedge[x0i], (float)yedge[y0i]);
            ImVec2 br((float)xedge[x1i], (float)yedge[y1i]);
            dl->AddRectFilled(tl, br, col);
        }

        // 4) Draw a 1px black border around the sampled window (min..max) using the same snapped edges.
        //    This gives you the visible frame around the sampled area even when min/max != 0/255.
        const int bx0 = std::clamp(g.b.minx, 0, 255);
        const int by0 = std::clamp(g.b.miny, 0, 255);
        const int bx1 = std::min(256, std::clamp(g.b.maxx, 0, 255) + step_x);
        const int by1 = std::min(256, std::clamp(g.b.maxy, 0, 255) + step_y);

        ImVec2 btl((float)xedge[bx0], (float)yedge[by0]);
        ImVec2 bbr((float)xedge[bx1], (float)yedge[by1]);
        dl->AddRect(btl, bbr, IM_COL32(0, 0, 0, 255), 0.0f, 0, 1.0f);
    }

    static std::vector<int> BuildLegendListAll(const std::vector<int>& all_deltas, int minNeg, int maxPos) {
        if (all_deltas.empty()) return {};
        std::vector<int> vals = all_deltas;
        std::sort(vals.begin(), vals.end());
        vals.erase(std::unique(vals.begin(), vals.end()), vals.end());

        auto has = [&](int v) { return std::binary_search(vals.begin(), vals.end(), v); };

        std::vector<int> out;
        auto ensure = [&](int v) {
            if (has(v) && std::find(out.begin(), out.end(), v) == out.end()) out.push_back(v);
            };

        // Always try to include anchors if present
        ensure(minNeg);
        ensure(0);
        ensure(maxPos);

        // Split remaining values
        std::vector<int> neg, pos;
        for (int v : vals) {
            if (std::find(out.begin(), out.end(), v) != out.end()) continue;
            if (v < 0) neg.push_back(v);
            else if (v > 0) pos.push_back(v);
        }

        int remaining = (int)neg.size() + pos.size();
        auto sample_into = [&](const std::vector<int>& src) {
            if (remaining <= 0 || src.empty()) return;
            if ((int)src.size() <= remaining) {
                for (int v : src) { out.push_back(v); if (--remaining == 0) break; }
                return;
            }
            // uniform sampling
            for (int i = 0; i < remaining; ++i) {
                int idx = (int)std::round(i * (src.size() - 1) / (float)(remaining - 1));
                out.push_back(src[(size_t)idx]);
            }
            remaining = 0;
            };

        // Prefer positives (where we emphasized hue+mod3), then negatives if space
        sample_into(pos);
        sample_into(neg);

        std::sort(out.begin(), out.end());
        return out;
    }

    static void DrawLegendTableAll(const std::vector<int>& all_deltas, int minNeg, int maxPos, float legend_width) {
        if (all_deltas.empty()) return;

        auto deltas = BuildLegendListAll(all_deltas, minNeg, maxPos);
        if (deltas.empty()) return;

        const float swatch = std::min(std::max(12.0f, legend_width / deltas.size()), 48.0f);
        // Leave a little side padding so it doesn't touch the borders
        const float usable = legend_width;
        int max_cols = (int)std::floor(usable / swatch);
        max_cols = std::clamp(max_cols, 3, 24);

        float used = swatch * deltas.size();
        float xpad = std::max(0.0f, (legend_width - used) * 0.5f);
        ImGui::SetCursorScreenPos(ImGui::GetCursorScreenPos() + ImVec2(xpad, 0));

        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0, 0));
        if (ImGui::BeginTable("legend_all", (int)deltas.size(),
            ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_NoHostExtendX | ImGuiTableFlags_NoBordersInBody))
        {
            for (size_t c = 0; c < deltas.size(); ++c)
                ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthFixed, swatch);

            // Row 1: swatches
            ImGui::TableNextRow();
            for (size_t c = 0; c < deltas.size(); ++c) {
                ImGui::TableSetColumnIndex((int)c);
                ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2(swatch, swatch));
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImU32 col = SeedDeltaToColorModB(deltas[c], minNeg, maxPos);
                dl->AddRectFilled(p, p + ImVec2(swatch, swatch), col);
                dl->AddRect(p, p + ImVec2(swatch, swatch), IM_COL32(20, 20, 20, 255));
            }

            // Row 2: labels
            ImGui::TableNextRow();
            for (size_t c = 0; c < deltas.size(); ++c) {
                ImGui::TableSetColumnIndex((int)c);
                ImGui::Text("%+d", deltas[c]);
            }
            ImGui::EndTable();
        }
        ImGui::PopStyleVar();
    }


    struct UIState {
        // left
        std::string search{};
        bool only_done{ false };
        int page_limit{ 50 };
        Page<SeedProbeLite> page{};
        std::optional<KeysetCursor> before{};
        std::optional<KeysetCursor> after{};
        int64_t selected_probe_id{ -1 };

        // memo/caches
        LazyCache<int64_t, SeedProbeRow> probe_cache;
        LazyCache<int64_t, std::string> savestate_filename_cache;
        LazyCache<int64_t, std::vector<DeltaSeedRow>> grid_cache;
        LazyCache<int64_t, std::vector<DeltaSeedRow>> unique_cache;
        std::unordered_map<GraphCacheKey, GridData, GraphCacheKeyHash> graph_cache;

        int legend_min_neg{ -2 };
        int legend_max_pos{ 32 };
        std::vector<int> legend_all_deltas; // raw deltas (we'll dedupe/sample per draw)

        // polling
        int64_t polling_probe_id{ -1 };
        int last_completed_count{ -1 };
        double next_poll_time_s{ 0.0 };
        bool poll_in_flight{ false };

        void reset_selection() {
            selected_probe_id = -1;
            polling_probe_id = -1;
            last_completed_count = -1;
            next_poll_time_s = 0.0;
            poll_in_flight = false;
        }
    };

    static UIState s;

    static std::mutex s_ui_apply_mtx;
    static std::vector<std::function<void()>> s_ui_apply_queue;

    static void enqueue_ui_apply(std::function<void()>&& fn) {
        std::lock_guard<std::mutex> lk(s_ui_apply_mtx);
        s_ui_apply_queue.push_back(std::move(fn));
    }

    static void drain_ui_apply_queue() {
        std::vector<std::function<void()>> work;
        {
            std::lock_guard<std::mutex> lk(s_ui_apply_mtx);
            work.swap(s_ui_apply_queue);
        }
        for (auto& fn : work) fn();
    }


    static bool is_selected_probe(int64_t probe_id) {
        return probe_id > 0 && s.selected_probe_id == probe_id;
    }

    static void invalidate_graph_cache_for_probe(int64_t probe_id) {
        s.graph_cache.erase(GraphCacheKey{ probe_id, simcore::ElementFamily::Main });
        s.graph_cache.erase(GraphCacheKey{ probe_id, simcore::ElementFamily::CStick });
        s.graph_cache.erase(GraphCacheKey{ probe_id, simcore::ElementFamily::Triggers });
    }

    static void fetch_page() {
        PagedQuery<> q;
        q.limit = s.page_limit;
        q.before = s.before;
        q.after = s.after;
        FutureQueue::Enqueue(
            SeedProbeRepo::ListPagedAsync(q, s.search, s.only_done),
            // on success
            [](DbResult<Page<SeedProbeLite>> r) {
                enqueue_ui_apply([r = std::move(r)]() mutable {
                    if (!r.ok) { GuiToastBus::Error("SeedProbe list error"); return; }
                    s.page = std::move(r.value);
                });
            },
            // on error
            [](std::exception_ptr e) {
                enqueue_ui_apply([]() {
                    GuiToastBus::Error("SeedProbe list error");
                });
            }
            );
    }

    static void ensure_savestate_filename_cached(int64_t savestate_id) {
        if (s.savestate_filename_cache.has(savestate_id)) return;
        FutureQueue::Enqueue(
            SavestateRepo::GetAsync(savestate_id),
            // on success
            [savestate_id](DbResult<std::optional<SavestateRow>> r) {
                if (!r.ok || !r.value) {
                    enqueue_ui_apply([]() {
                        GuiToastBus::Error("SeedProbe savestate not found");
                    });
                    return;
                }
                int64_t obj = r.value->object_ref_id;
                FutureQueue::Enqueue(
                    ObjectStore::GetAsync(obj),
                    // on success
                    [savestate_id](DbResult<ObjectRefRow> rr) {
                        enqueue_ui_apply([savestate_id, rr = std::move(rr)]() mutable {
                            if (rr.ok) s.savestate_filename_cache.set(savestate_id, rr.value.filename);
                            else GuiToastBus::Error("SeedProbe savestate object not found");
                        });
                    },
                    // on error
                    [](std::exception_ptr e) {
                        enqueue_ui_apply([]() {
                            GuiToastBus::Error("SeedProbe object_ref error");
                        });
                    }
                );
            },
            // on error
            [](std::exception_ptr e) {
                enqueue_ui_apply([]() {
                    GuiToastBus::Error("SeedProbe savestate_id error");
                });
            }
        );
    }

    static void ensure_probe_cached(int64_t probe_id) {
        if (s.probe_cache.has(probe_id)) return;
        FutureQueue::Enqueue(
            SeedProbeRepo::GetAsync(probe_id),
            // on success
            [probe_id](DbResult<SeedProbeRow> r) {
                enqueue_ui_apply([probe_id, r = std::move(r)]() mutable {
                    if (r.ok) s.probe_cache.set(probe_id, r.value);
                    else GuiToastBus::Error("Unable to get SeedProbeRow");
                });
            },
            // on error
            [](std::exception_ptr e) {
                enqueue_ui_apply([]() {
                    GuiToastBus::Error("Exception on getting SeedProbeRow");
                });
            }
        );
    }

    static void ensure_lists_cached(int64_t probe_id) {
        if (!s.grid_cache.has(probe_id)) {
            FutureQueue::Enqueue(
                DeltaSeedRepo::ListGridForProbeAsync(probe_id),
                // on success
                [probe_id](DbResult<std::vector<DeltaSeedRow>> r) {
                    enqueue_ui_apply([probe_id, r = std::move(r)]() mutable {
                        if (r.ok) s.grid_cache.set(probe_id, r.value);
                        else GuiToastBus::Error("Unable to get grid DeltaSeedRow list");
                    });
                },
                // on error
                [](std::exception_ptr e) {
                    enqueue_ui_apply([]() {
                        GuiToastBus::Error("Exception on getting grid DeltaSeedRow list");
                    });
                }
            );
        }
        if (!s.unique_cache.has(probe_id)) {
            FutureQueue::Enqueue(
                DeltaSeedRepo::ListUniqueForProbeAsync(probe_id),
                // on success
                [probe_id](DbResult<std::vector<DeltaSeedRow>> r) {
                    enqueue_ui_apply([probe_id, r = std::move(r)]() mutable {
                        if (r.ok) s.unique_cache.set(probe_id, r.value);
                        else GuiToastBus::Error("Unable to get unique DeltaSeedRow list");
                    });
                },
                // on error
                [](std::exception_ptr e) {
                    enqueue_ui_apply([]() {
                        GuiToastBus::Error("Exception on getting unique DeltaSeedRow list");
                    });
                }
            );
        }
    }

    static void force_refresh_probe_data(int64_t probe_id) {
        FutureQueue::Enqueue(
            SeedProbeRepo::GetAsync(probe_id),
            [probe_id](DbResult<SeedProbeRow> r) {
                enqueue_ui_apply([probe_id, r = std::move(r)]() mutable {
                    if (!r.ok) {
                        GuiToastBus::Error("Unable to refresh SeedProbeRow");
                        return;
                    }
                    if (!is_selected_probe(probe_id)) return;
                    s.probe_cache.set(probe_id, r.value);
                });
            },
            [](std::exception_ptr) {
                enqueue_ui_apply([]() {
                    GuiToastBus::Error("Exception while refreshing SeedProbeRow");
                });
            }
        );

        FutureQueue::Enqueue(
            DeltaSeedRepo::ListGridForProbeAsync(probe_id),
            [probe_id](DbResult<std::vector<DeltaSeedRow>> r) {
                enqueue_ui_apply([probe_id, r = std::move(r)]() mutable {
                    if (!r.ok) {
                        GuiToastBus::Error("Unable to refresh grid DeltaSeed rows");
                        return;
                    }
                    if (!is_selected_probe(probe_id)) return;
                    s.grid_cache.set(probe_id, r.value);
                    invalidate_graph_cache_for_probe(probe_id);
                    s.last_completed_count = (int)r.value.size();
                });
            },
            [](std::exception_ptr) {
                enqueue_ui_apply([]() {
                    GuiToastBus::Error("Exception while refreshing grid DeltaSeed rows");
                });
            }
        );

        FutureQueue::Enqueue(
            DeltaSeedRepo::ListUniqueForProbeAsync(probe_id),
            [probe_id](DbResult<std::vector<DeltaSeedRow>> r) {
                enqueue_ui_apply([probe_id, r = std::move(r)]() mutable {
                    if (!r.ok) {
                        GuiToastBus::Error("Unable to refresh unique DeltaSeed rows");
                        return;
                    }
                    if (!is_selected_probe(probe_id)) return;
                    s.unique_cache.set(probe_id, r.value);
                });
            },
            [](std::exception_ptr) {
                enqueue_ui_apply([]() {
                    GuiToastBus::Error("Exception while refreshing unique DeltaSeed rows");
                });
            }
        );
    }

    static void poll_selected_probe_if_due() {
        if (s.selected_probe_id <= 0) {
            s.polling_probe_id = -1;
            s.poll_in_flight = false;
            return;
        }

        auto pr = s.probe_cache.get(s.selected_probe_id);
        if (!pr || pr->status != "running") {
            s.polling_probe_id = -1;
            s.poll_in_flight = false;
            return;
        }

        if (s.polling_probe_id != s.selected_probe_id) {
            s.polling_probe_id = s.selected_probe_id;
            s.last_completed_count = -1;
            s.next_poll_time_s = 0.0;
            s.poll_in_flight = false;
        }

        const double now = ImGui::GetTime();
        if (s.poll_in_flight || now < s.next_poll_time_s) return;

        const int64_t probe_id = s.selected_probe_id;
        s.poll_in_flight = true;
        s.next_poll_time_s = now + 2.0;

        FutureQueue::Enqueue(
            SeedProbeRepo::GetAsync(probe_id),
            [probe_id](DbResult<SeedProbeRow> rowr) {
                if (!rowr.ok || rowr.value.status != "running") {
                    enqueue_ui_apply([probe_id, rowr = std::move(rowr)]() mutable {
                        if (!is_selected_probe(probe_id)) {
                            s.poll_in_flight = false;
                            return;
                        }
                        if (!rowr.ok) {
                            GuiToastBus::Error("Unable to poll selected seedprobe");
                            s.poll_in_flight = false;
                            return;
                        }

                        s.probe_cache.set(probe_id, rowr.value);
                        force_refresh_probe_data(probe_id);
                        s.poll_in_flight = false;
                    });
                    return;
                }

                enqueue_ui_apply([probe_id, rowr = std::move(rowr)]() mutable {
                    if (!is_selected_probe(probe_id)) {
                        s.poll_in_flight = false;
                        return;
                    }
                    s.probe_cache.set(probe_id, rowr.value);
                });

                FutureQueue::Enqueue(
                    DeltaSeedRepo::ListGridForProbeAsync(probe_id),
                    [probe_id](DbResult<std::vector<DeltaSeedRow>> gridr) {
                        enqueue_ui_apply([probe_id, gridr = std::move(gridr)]() mutable {
                            if (!is_selected_probe(probe_id)) {
                                s.poll_in_flight = false;
                                return;
                            }
                            if (!gridr.ok) {
                                GuiToastBus::Error("Unable to poll grid DeltaSeed rows");
                                s.poll_in_flight = false;
                                return;
                            }

                            const int completed_count = (int)gridr.value.size();
                            const bool progressed = (s.last_completed_count < 0) || (completed_count > s.last_completed_count);
                            s.last_completed_count = completed_count;

                            if (!progressed) {
                                s.poll_in_flight = false;
                                return;
                            }

                            s.grid_cache.set(probe_id, gridr.value);
                            invalidate_graph_cache_for_probe(probe_id);
                        });

                        FutureQueue::Enqueue(
                            DeltaSeedRepo::ListUniqueForProbeAsync(probe_id),
                            [probe_id](DbResult<std::vector<DeltaSeedRow>> uniqr) {
                                enqueue_ui_apply([probe_id, uniqr = std::move(uniqr)]() mutable {
                                    if (is_selected_probe(probe_id) && uniqr.ok) {
                                        s.unique_cache.set(probe_id, uniqr.value);
                                    }
                                    else if (is_selected_probe(probe_id) && !uniqr.ok) {
                                        GuiToastBus::Error("Unable to poll unique DeltaSeed rows");
                                    }
                                    s.poll_in_flight = false;
                                });
                            },
                            [](std::exception_ptr) {
                                enqueue_ui_apply([]() {
                                    GuiToastBus::Error("Exception while polling unique DeltaSeed rows");
                                    s.poll_in_flight = false;
                                });
                            }
                        );
                    },
                    [](std::exception_ptr) {
                        enqueue_ui_apply([]() {
                            GuiToastBus::Error("Exception while polling grid DeltaSeed rows");
                            s.poll_in_flight = false;
                        });
                    }
                );
            },
            [](std::exception_ptr) {
                enqueue_ui_apply([]() {
                    GuiToastBus::Error("Exception while polling selected seedprobe");
                    s.poll_in_flight = false;
                });
            }
        );
    }

    static void build_graph_cache_if_needed(int64_t probe_id) {
        auto gopt = s.grid_cache.get(probe_id); if (!gopt) return;
        const auto& rows = *gopt;
        std::vector<GridPoint> main_pts, c_pts, trig_pts;
        int minNeg = -2, maxPos = 32;
        for (auto& r : rows) {
            simcore::ElementFamily fam = (simcore::ElementFamily)r.input.get_family();
            GridPoint gp{};
            gp.delta = r.seed_delta;
            minNeg = std::min(minNeg, r.seed_delta);
            maxPos = std::max(maxPos, r.seed_delta);
            if (fam == simcore::ElementFamily::Main) { gp.x = r.input.main_x; gp.y = r.input.main_y; main_pts.push_back(gp); }
            else if (fam == simcore::ElementFamily::CStick) { gp.x = r.input.c_x; gp.y = r.input.c_y; c_pts.push_back(gp); }
            else if (fam == simcore::ElementFamily::Triggers) { gp.x = r.input.trig_l; gp.y = r.input.trig_r; trig_pts.push_back(gp); }
        }

        // Combine bounds for Main & C
        auto make_gd = [&](simcore::ElementFamily f, std::vector<GridPoint>& pts)->GridData {
            GridData gd; gd.pts = std::move(pts);
            gd.b = infer_bounds(gd.pts);
            gd.step = infer_step(gd.pts);
            gd.minNeg = minNeg; gd.maxPos = maxPos;
            return gd;
            };

        GridData main_gd = make_gd(simcore::ElementFamily::Main, main_pts);
        GridData c_gd = make_gd(simcore::ElementFamily::CStick, c_pts);
        GridData t_gd = make_gd(simcore::ElementFamily::Triggers, trig_pts);

        // Equalize bounds for Main/C if either exists
        if (!main_gd.pts.empty() || !c_gd.pts.empty()) {
            Bounds b{};
            bool inited = false;
            auto acc = [&](const GridData& g) {
                if (g.pts.empty()) return;
                if (!inited) { b = g.b; inited = true; }
                else {
                    b.minx = std::min(b.minx, g.b.minx);
                    b.maxx = std::max(b.maxx, g.b.maxx);
                    b.miny = std::min(b.miny, g.b.miny);
                    b.maxy = std::max(b.maxy, g.b.maxy);
                }
                };
            acc(main_gd); acc(c_gd);
            if (!main_gd.pts.empty()) main_gd.b = b;
            if (!c_gd.pts.empty())    c_gd.b = b;
        }

        auto put = [&](simcore::ElementFamily f, GridData&& gd) {
            GraphCacheKey k{ probe_id, f };
            s.graph_cache.erase(k);
            s.graph_cache.emplace(k, std::move(gd));
            };
        if (!main_gd.pts.empty()) put(simcore::ElementFamily::Main, std::move(main_gd));
        if (!c_gd.pts.empty())    put(simcore::ElementFamily::CStick, std::move(c_gd));
        if (!t_gd.pts.empty())    put(simcore::ElementFamily::Triggers, std::move(t_gd));

        s.legend_min_neg = minNeg;
        s.legend_max_pos = maxPos;
        s.legend_all_deltas.clear();
        s.legend_all_deltas.reserve(rows.size());
        for (const auto& r : rows) s.legend_all_deltas.push_back(r.seed_delta);
    }

    static void draw_left_list() {
        ImGui::TextUnformatted("Seed Probes");
        ImGui::Separator();

        ImGui::PushItemWidth(-1);
        ImGui::InputText("##search", &s.search);
        ImGui::PopItemWidth();
        ImGui::SameLine();
        ImGui::Checkbox("done only", &s.only_done);
        ImGui::SameLine();
        if (ImGui::Button("Search")) { s.before.reset(); s.after.reset(); fetch_page(); }

        if (ImGui::BeginTable("seedprobes", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 100);
            ImGui::TableSetupColumn("Savestate ID", ImGuiTableColumnFlags_WidthFixed, 100);
            ImGui::TableSetupColumn("Filename", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            for (auto& r : s.page.items) {
                ensure_savestate_filename_cached(r.savestate_id);
                ImGui::PushID((int)r.id);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);

                ImVec2 row_top = ImGui::GetCursorScreenPos();

                ImGui::TextUnformatted(std::to_string(r.id).c_str());

                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(r.status.c_str());

                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%lld", (long long)r.savestate_id);

                ImGui::TableSetColumnIndex(3);
                {
                    auto fn = s.savestate_filename_cache.get(r.savestate_id);
                    if (fn) ImGui::TextUnformatted(fn->c_str());
                }

                bool sel = (s.selected_probe_id == r.id);
                ImGui::SetCursorScreenPos(row_top);
                const float row_h = ImGui::GetTextLineHeightWithSpacing();
                if (ImGui::Selectable("##Row", 
                    sel, 
                    ImGuiSelectableFlags_SpanAllColumns,
                    ImVec2(0, row_h))) 
                {
                    const bool selection_changed = (s.selected_probe_id != r.id);
                    s.selected_probe_id = r.id;
                    if (selection_changed) {
                        s.polling_probe_id = r.id;
                        s.last_completed_count = -1;
                        s.next_poll_time_s = 0.0;
                        s.poll_in_flight = false;
                        force_refresh_probe_data(r.id);
                    }
                    ensure_probe_cached(r.id);
                    ensure_lists_cached(r.id);
                }

                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        ImGui::Separator();
        if (ImGui::Button("Prev") && s.page.prev) { s.before = s.page.prev; s.after.reset(); fetch_page(); }
        ImGui::SameLine();
        if (ImGui::Button("Next") && s.page.next) { s.after = s.page.next; s.before.reset(); fetch_page(); }
    }

    static void draw_header_kv(const char* k, const std::string& v) {
        ImGui::TextUnformatted(k); ImGui::SameLine();
        ImGui::TextDisabled(":"); ImGui::SameLine();
        ImGui::TextUnformatted(v.c_str());
    }

    static void draw_right_details() {
        if (s.selected_probe_id <= 0) {
            ImGui::TextDisabled("Select a SeedProbe on the left");
            return;
        }

        auto pr = s.probe_cache.get(s.selected_probe_id);
        if (!pr) { ImGui::TextDisabled("Loading probe"); return; }

        auto sav = SavestateRepo::GetByProbeId(s.selected_probe_id);
        std::string ss_name;
        if (sav.ok && sav.value) {
            auto rr = ObjectStore::Get(sav.value->object_ref_id);
            if (rr.ok) ss_name = rr.value.filename;
        }

        // Section A: neutral RNG + details
        if (ImGui::BeginChild("A", ImVec2(0, 80), true)) {
            std::string seed_hex = pr->neutral_seed >= 0 ? ("0x" + hex32((uint32_t)pr->neutral_seed)) : "n/a";
            draw_header_kv("Neutral RNG", seed_hex);
            ImGui::SameLine(); ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical); ImGui::SameLine();
            draw_header_kv("Probe ID", std::to_string(pr->id));
            ImGui::SameLine(); ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical); ImGui::SameLine();
            draw_header_kv("Status", pr->status);
            ImGui::SameLine(); ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical); ImGui::SameLine();
            draw_header_kv("Codec", std::to_string(pr->codec_version));
            ImGui::SameLine(); ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical); ImGui::SameLine();
            draw_header_kv("Savestate", ss_name.empty() ? std::to_string(pr->savestate_id) : ss_name);
        }
        ImGui::EndChild();

        // Section B: 3 side-by-side graphs (square)
        auto gopt = s.grid_cache.get(s.selected_probe_id);
        if (gopt && !gopt->empty()) {
            build_graph_cache_if_needed(s.selected_probe_id);

            if (ImGui::BeginChild("B", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY)) {
                ImVec2 avail = ImGui::GetContentRegionAvail();
                const float gap = ImGui::GetStyle().ItemSpacing.x;
                const float per = (avail.x - 2.0f * gap) / 3.0f;        // width per column
                const float side = floorf(std::max(16.0f, per));

                const char* labels[3] = { "Main Stick", "C-Stick", "Triggers" };
                simcore::ElementFamily fams[3] = { simcore::ElementFamily::Main, simcore::ElementFamily::CStick, simcore::ElementFamily::Triggers };

                for (int i = 0; i < 3; ++i) {
                    if (i > 0) ImGui::SameLine(0.0f, gap);
                    ImGui::BeginGroup();

                    // Title
                    ImGui::TextUnformatted(labels[i]);

                    // Reserve the full column footprint (per × side), then center the square inside it
                    ImVec2 column_tl = ImGui::GetCursorScreenPos();
                    ImGui::Dummy(ImVec2(per, side));

                    // Square placement
                    ImVec2 plot_tl = column_tl + ImVec2((per - side) * 0.5f, 0.0f);
                    ImVec2 plot_sz = ImVec2(side, side);

                    GraphCacheKey k{ s.selected_probe_id, fams[i] };
                    auto it = s.graph_cache.find(k);
                    if (it != s.graph_cache.end()) {
                        draw_grid_rects(it->second, plot_tl, plot_sz);
                    }
                    else {
                        ImDrawList* dl = ImGui::GetWindowDrawList();
                        dl->AddRectFilled(plot_tl, plot_tl + plot_sz, IM_COL32(15, 15, 15, 255));
                        dl->AddText(plot_tl + ImVec2(8, 8), IM_COL32(200, 200, 200, 255), "no data");
                    }

                    ImGui::EndGroup();
                }
                ImGui::Dummy(ImVec2(0, 6));

                // Draw a single combined legend centered under all graphs
                ImVec2 avail2 = ImGui::GetContentRegionAvail();
                float total_width = (avail.x); // reuse the 'avail' from above if still in scope
                ImGui::PushID(0xBADB01); // avoid ID collisions
                DrawLegendTableAll(s.legend_all_deltas, s.legend_min_neg, s.legend_max_pos, total_width);
                ImGui::PopID();
            }
            ImGui::EndChild();
        }
        else {
            ImGui::BeginChild("B", ImVec2(0, 80), true);
            ImGui::TextDisabled("No grid rows");
            ImGui::EndChild();
        }

        // Section C: unique table (latest wins)
        if (ImGui::BeginChild("C", ImVec2(0, 0), true)) {
            auto uopt = s.unique_cache.get(s.selected_probe_id);
            if (!uopt) {
                ImGui::TextDisabled("Loading unique rows");
            }
            else {
                const bool have_neutral = (pr->neutral_seed >= 0);
                const uint32_t neutral_u32 = (uint32_t)pr->neutral_seed;
                const DeltaSeedRow neutral_row{ .input = GCInputFrame{} };

                // latest-wins per resulting seed, then materialize to a vector for sorting
                std::unordered_map<uint32_t, const DeltaSeedRow*> latest_by_seed;
                if (have_neutral) {
                    for (auto& r : *uopt) {
                        // (neutral + delta) mod 2^32
                        uint32_t seed = (uint32_t)((uint64_t)neutral_u32 + (int64_t)r.seed_delta);
                        auto it = latest_by_seed.find(seed);
                        if (it == latest_by_seed.end() || r.id > it->second->id)
                            latest_by_seed[seed] = &r;
                    }
                }

                std::vector<std::pair<uint32_t, const DeltaSeedRow*>> items;
                items.reserve(latest_by_seed.size());
                for (auto& kv : latest_by_seed) items.emplace_back(kv.first, kv.second);
                items.emplace_back(neutral_u32, &neutral_row);

                std::sort(items.begin(), items.end(),
                    [](const auto& a, const auto& b) { return a.first < b.first; });

                if (ImGui::BeginTable("unique", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchSame)) {
                    ImGui::TableSetupColumn("Input");
                    ImGui::TableSetupColumn("Seed");
                    ImGui::TableHeadersRow();

                    // Sorted unique rows (by resulting seed)
                    if (!have_neutral) {
                        // We cant compute resulting seeds without neutral; show a hint.
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("Unique results require a neutral seed.");
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("n/a");
                    }
                    else {
                        for (auto& [seed, row] : items) {
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::TextUnformatted(DescribeFrameCompact(row->input).c_str());
                            ImGui::TableNextColumn();
                            std::string hex = std::string("0x") + hex32(seed);
                            ImGui::TextUnformatted(hex.c_str());
                        }
                    }
                    poll_selected_probe_if_due();

                    drain_ui_apply_queue();

                    ImGui::EndTable();
                }
            }
        }
        ImGui::EndChild();
    }

} // anon

void SeedProbePane::OnActivated() {
    s.reset_selection();
    s.before.reset(); s.after.reset(); s.page.items.clear(); s.page.prev.reset(); s.page.next.reset();
    fetch_page();
}

void SeedProbePane::Draw() {
    ImGui::Begin("SeedProbe Pane");

    drain_ui_apply_queue();

    ImVec2 full = ImGui::GetContentRegionAvail();
    float leftW = std::max(300.0f, full.x * 0.38f);
    float sep = 8.0f;
    ImGui::BeginChild("left", ImVec2(leftW, 0), true);
    draw_left_list();
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::Dummy(ImVec2(sep, 0));
    ImGui::SameLine();

    ImGui::BeginChild("right", ImVec2(0, 0), true);
    draw_right_details();
    ImGui::EndChild();

    ImGui::End();
}
