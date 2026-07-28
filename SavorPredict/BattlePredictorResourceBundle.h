#pragma once

#include "ActionViewSelectorModel.h"
#include "CombatantVisualDispatcherModel.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace savor::predict {

enum class BattlePredictorResourceInputStatus {
    Ready,
    MissingInput,
    Ambiguous,
};

enum class BattlePredictorResourceProviderKind {
    DirectSpice,
    LegacyStdJsonDirectMld,
};

enum class BattlePredictorResourceDiagnosticSeverity {
    Info,
    Warning,
    Error,
};

struct BattlePredictorResourceDiagnostic {
    BattlePredictorResourceDiagnosticSeverity severity =
        BattlePredictorResourceDiagnosticSeverity::Info;
    std::string logical_role;
    std::string message;
    std::optional<std::uint32_t> source_offset;
};

struct BattlePredictorResourceSourceIdentity {
    std::string logical_role;
    std::string relative_path;
    std::string normalized_relative_path;
    std::string source_path;
    std::uint64_t size_bytes = 0;
    std::string sha256;
    std::string parser_identity;
    std::string parser_status;
    std::vector<BattlePredictorResourceDiagnostic> diagnostics;
};

struct BattlePredictorMldMotionSlot {
    std::size_t table_index = 0;
    std::uint32_t source_entry_id = 0;
    std::size_t motion_slot = 0;
    std::uint32_t source_motion_address = 0;
    bool has_motion_address = false;
    bool decoded = false;
    std::optional<std::uint32_t> declared_frame_count;
    std::optional<std::uint32_t> source_object_address;
    std::optional<std::uint32_t> node_count;
    std::optional<bool> short_rotation;
    std::string provenance;
};

struct BattlePredictorMldEntryCatalog {
    std::size_t table_index = 0;
    std::uint32_t source_entry_id = 0;
    std::vector<BattlePredictorMldMotionSlot> motion_slots;
    std::size_t declared_nonzero_motion_slots = 0;
    std::size_t decoded_nonzero_motion_slots = 0;
    bool complete = false;
};

struct BattlePredictorResourceTemplate {
    std::string resource_stem;
    CombatantVisualResource visual_resource;
    Std0Table companion_std0_table;
    Std0Table runtime_aux_table;
    bool runtime_aux_table_has_action_row_prefix = false;
    int runtime_aux_table_prefix_rows = 0;
    std::vector<BattlePredictorMldEntryCatalog> mld_entries;
    BattlePredictorResourceInputStatus status =
        BattlePredictorResourceInputStatus::MissingInput;
    std::vector<BattlePredictorResourceDiagnostic> diagnostics;
};

struct AsciiCaseInsensitiveLess {
    using is_transparent = void;

    bool operator()(std::string_view lhs, std::string_view rhs) const noexcept;
};

using BattlePredictorResourceTemplateMap =
    std::map<std::string, BattlePredictorResourceTemplate, AsciiCaseInsensitiveLess>;

struct BattlePredictorResourceBundle {
    static constexpr std::string_view kAdapterVersion =
        "savor_spice_battle_content_adapter_v1";

    BattlePredictorResourceInputStatus status =
        BattlePredictorResourceInputStatus::MissingInput;
    BattlePredictorResourceProviderKind provider_kind =
        BattlePredictorResourceProviderKind::DirectSpice;
    std::string adapter_version{ kAdapterVersion };
    std::string spice_revision;
    BattlePredictorResourceTemplateMap resource_templates;
    std::optional<CombatantVisualResource> target_reaction_effect_resource;
    std::vector<BattlePredictorResourceSourceIdentity> sources;
    std::vector<BattlePredictorResourceDiagnostic> diagnostics;
    std::string bundle_digest;

    [[nodiscard]] const BattlePredictorResourceTemplate* find_resource(
        std::string_view resource_stem) const noexcept;
};

using BattlePredictorResourceBundlePtr =
    std::shared_ptr<const BattlePredictorResourceBundle>;

[[nodiscard]] const char* battle_predictor_resource_input_status_name(
    BattlePredictorResourceInputStatus status);

[[nodiscard]] const char* battle_predictor_resource_provider_kind_name(
    BattlePredictorResourceProviderKind kind);

} // namespace savor::predict
