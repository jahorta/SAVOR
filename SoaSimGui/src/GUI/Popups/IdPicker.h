#pragma once
#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <cstdint>
#include "DB/Querying/Paging.h"
#include "DB/Querying/PagedQuery.h"
#include "DB/Querying/SnapshotMailbox.h"

namespace soasim::ui {

    enum class LedgerKind {
        Savestate,
        ObjectRef,
        BattleRunGroup,
        SeedProbe,
        TasMovie,
        ExplorerSettings
    };

    template <class Row>
    struct LedgerColumn {
        const char* label{};
        float width_px{ 0.f }; // 0 = auto
        std::function<std::string(const Row&)> format;
    };

    template <class Row>
    struct LedgerAdapter {
        LedgerKind kind{};
        const char* title{};
        int page_size{ 100 };

        std::vector<LedgerColumn<Row>> columns;

        // Issue/refresh a page request (runs via DataService -> DBService).
        std::function<void(const PagedQuery<>& q, std::string_view search)> submit_request;

        // Read the freshest snapshot (non-blocking).
        std::function<std::optional<Page<Row>>()> poll_snapshot;

        // Row -> primary id
        std::function<int64_t(const Row&)> get_id;

        // Optional right-side preview renderer (no-op OK).
        std::function<void(const Row&)> draw_preview;

        // Optional nested action (e.g., "Filter by Savestate..." for SeedProbe).
        // Implementer may open another picker and then internally update future submit_request calls.
        std::function<void()> open_aux_filter;
    };

    struct PickerOpenArgs {
        const char* modal_id{ "IdPicker" };
        float width{ 900.f };
        float height{ 600.f };
        std::string initial_search;
        PagedQuery<> initial_query; // usually page_size + order desc
    };

    struct PickResult {
        bool ok{ false };
        int64_t id{ 0 };
    };

    // Stateful modal. Call Draw() each frame while open==true.
    template <class Row>
    struct LedgerPicker {
        LedgerAdapter<Row> adapter;
        PickerOpenArgs args;

        bool open{ false };
        std::optional<Page<Row>> page;
        std::optional<Row> current_row;
        int64_t selected_id{ 0 };
        std::string search_buffer;

        // Renders the modal; invokes cb when user clicks OK or double-clicks a row.
        // Returns true once the modal is closed.
        using Callback = std::function<void(const PickResult&, const std::optional<Row>&)>;
        bool Draw(Callback cb);
    };

} // namespace soasim::ui
