#pragma once

#include "BattlePredictorResourceBundle.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace savor::predict::adapter_test {

struct MldCatalogVariantInput {
    std::uint32_t node_count = 0;
    bool short_rotation = false;
    bool has_motion = false;
    std::uint32_t declared_frame_count = 0;
};

struct MldCatalogResourceInput {
    std::uint32_t source_motion_address = 0;
    std::vector<MldCatalogVariantInput> variants;
};

struct MldCatalogBindingInput {
    std::size_t table_index = 0;
    std::uint32_t source_entry_id = 0;
    std::size_t motion_slot = 0;
    std::uint32_t source_motion_address = 0;
    std::uint32_t source_object_address = 0;
    std::uint32_t node_count = 0;
    bool short_rotation = false;
    std::size_t variant_index = 0;
};

struct MldCatalogEntryInput {
    std::size_t table_index = 0;
    std::uint32_t source_entry_id = 0;
    bool has_motion_address_list = true;
    std::vector<std::uint32_t> motion_addresses;
    std::size_t declared_nonzero_motion_slots = 0;
};

struct MldCatalogDiagnosticInput {
    BattlePredictorResourceDiagnosticSeverity severity =
        BattlePredictorResourceDiagnosticSeverity::Info;
    std::string message;
    std::optional<std::uint32_t> source_offset;
};

struct MldCatalogProjectionInput {
    std::string logical_role;
    std::vector<MldCatalogEntryInput> entries;
    std::vector<MldCatalogResourceInput> resources;
    std::vector<MldCatalogBindingInput> bindings;
    std::vector<MldCatalogDiagnosticInput> diagnostics;
};

struct MldCatalogProjectionDiagnostic {
    BattlePredictorResourceInputStatus classification =
        BattlePredictorResourceInputStatus::Ready;
    BattlePredictorResourceDiagnostic diagnostic;
};

struct MldCatalogProjectionResult {
    BattlePredictorResourceInputStatus status =
        BattlePredictorResourceInputStatus::Ready;
    std::vector<BattlePredictorMldEntryCatalog> entries;
    std::vector<CombatantVisualMotionFrameCount> motion_frame_counts;
    std::vector<MldCatalogProjectionDiagnostic> diagnostics;
};

[[nodiscard]] MldCatalogProjectionResult project_mld_motion_catalog(
    const MldCatalogProjectionInput& input);

struct CaseInsensitiveCandidateSelection {
    BattlePredictorResourceInputStatus status =
        BattlePredictorResourceInputStatus::MissingInput;
    std::optional<std::size_t> selected_index;
};

[[nodiscard]] CaseInsensitiveCandidateSelection
select_case_insensitive_candidate(
    std::string_view expected_name,
    std::span<const std::string> candidate_names);

} // namespace savor::predict::adapter_test
